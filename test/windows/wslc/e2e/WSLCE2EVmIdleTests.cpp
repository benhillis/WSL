/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2EVmIdleTests.cpp

Abstract:

    End-to-end tests for on-demand / idle-terminating wslc VMs. A session's backing VM is
    created lazily on the first VM-requiring operation and torn down again once there are no
    active (Created or Running) containers and no in-flight operations, while the per-user
    session survives across VM restarts. VM lifecycle is observed via
    IWSLCSession::GetVmDiagnostics, which reads state without bringing the VM up.

--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"
#include <WSLCProcessLauncher.h>

namespace WSLCE2ETests {
using namespace wsl::shared;

class WSLCE2EVmIdleTests
{
    WSLC_TEST_CLASS(WSLCE2EVmIdleTests)

    const TestImage& AlpineImage = AlpineTestImage();

    static WSLCVmDiagnostics QueryDiagnostics(const TestSession& session)
    {
        WSLCVmDiagnostics diagnostics{};
        VERIFY_SUCCEEDED(session.Session().GetVmDiagnostics(&diagnostics));
        return diagnostics;
    }

    // Polls VM diagnostics until the VM reaches the desired running state. Idle teardown and
    // on-demand bring-up happen asynchronously, so callers must wait rather than assume.
    static void WaitForVmRunningState(const TestSession& session, bool running)
    {
        retry::RetryWithTimeout<void>(
            [&]() {
                const auto diagnostics = QueryDiagnostics(session);
                THROW_HR_IF(E_FAIL, static_cast<bool>(diagnostics.Running) != running);
            },
            std::chrono::milliseconds(250),
            std::chrono::seconds(60));
    }

    // A freshly created session has no VM until the first VM-requiring operation arrives, and
    // the VM idle-terminates once that operation completes with nothing left active.
    WSLC_TEST_METHOD(WSLCE2E_VmIdle_LazyStartAndIdleStop)
    {
        auto session = TestSession::Create(L"wslc-vmidle-lazy");

        const auto initial = QueryDiagnostics(session);
        VERIFY_IS_FALSE(static_cast<bool>(initial.Running));
        VERIFY_ARE_EQUAL(initial.StartCount, 0ul);

        // The first VM-requiring operation brings the VM up on demand.
        EnsureImageIsLoaded(AlpineImage, session.Name());

        // With no Created/Running containers and no in-flight operations, the VM tears down.
        WaitForVmRunningState(session, false);

        const auto afterIdle = QueryDiagnostics(session);
        VERIFY_IS_TRUE(afterIdle.StartCount >= 1ul);
    }

    // After the VM idle-terminates, a subsequent operation recreates it from scratch and any
    // previously loaded images remain available (storage persists across VM restarts).
    WSLC_TEST_METHOD(WSLCE2E_VmIdle_RecreateOnDemandAndPersistState)
    {
        auto session = TestSession::Create(L"wslc-vmidle-recreate");

        EnsureImageIsLoaded(AlpineImage, session.Name());
        WaitForVmRunningState(session, false);
        const auto startCountBeforeRecreate = QueryDiagnostics(session).StartCount;

        // Running a container recreates the VM, runs to completion, then idles again.
        RunWslcAndVerify(
            std::format(L"container run --session {} --rm {} echo hello", session.Name(), AlpineImage.NameAndTag()),
            {.Stderr = L"", .ExitCode = 0});

        WaitForVmRunningState(session, false);
        const auto startCountAfterRecreate = QueryDiagnostics(session).StartCount;
        VERIFY_IS_TRUE(startCountAfterRecreate > startCountBeforeRecreate);

        // The image loaded before the restart survived the VM teardown/recreate cycle.
        auto images = RunWslc(std::format(L"image list --session {}", session.Name()));
        images.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_TRUE(images.StdoutContainsSubstring(L"alpine"));
    }

    // A container in the Created state (created but never started) counts as active and keeps
    // the VM alive; removing it lets the VM idle-terminate.
    WSLC_TEST_METHOD(WSLCE2E_VmIdle_CreatedContainerKeepsVmAlive)
    {
        constexpr auto containerName = L"wslc-vmidle-created";
        auto session = TestSession::Create(L"wslc-vmidle-created-session");

        EnsureImageIsLoaded(AlpineImage, session.Name());

        RunWslcAndVerify(
            std::format(L"container create --session {} --name {} {} sleep 3600", session.Name(), containerName, AlpineImage.NameAndTag()),
            {.Stderr = L"", .ExitCode = 0});

        // The VM must stay up while a Created container exists, even with nothing running.
        WaitForVmRunningState(session, true);
        std::this_thread::sleep_for(std::chrono::seconds(3));
        VERIFY_IS_TRUE(static_cast<bool>(QueryDiagnostics(session).Running));

        // Removing the only container drops the active count to zero and the VM idles.
        RunWslcAndVerify(std::format(L"container rm --session {} {}", session.Name(), containerName), {.Stderr = L"", .ExitCode = 0});

        WaitForVmRunningState(session, false);
    }

    // A long-lived root-namespace process (created via CreateRootNamespaceProcess) is not tracked
    // as a container, so it does not contribute to the active-container check. It must nonetheless
    // keep the VM alive for as long as the client holds the returned process, via the activity
    // token bound to the process's lifetime. Without that token the idle worker would tear the VM
    // down once the grace period elapsed, killing the process out from under the client.
    WSLC_TEST_METHOD(WSLCE2E_VmIdle_RootProcessKeepsVmAlive)
    {
        auto session = TestSession::Create(L"wslc-vmidle-rootproc");

        // Launch a long-running root-namespace process and keep the returned process object alive.
        // This brings the VM up on demand to host the process.
        wsl::windows::common::WSLCProcessLauncher launcher("/bin/sleep", {"/bin/sleep", "3600"});
        std::optional<wsl::windows::common::ClientRunningWSLCProcess> process = launcher.Launch(*session.Session());

        WaitForVmRunningState(session, true);

        // The VM must remain running past the idle grace period (30s) while the process is held,
        // even though there are no containers and no in-flight operations. Without the keep-alive
        // token the idle worker would have torn the VM down ~30s after the creating call returned,
        // so a generous margin past the grace period reliably catches that regression.
        std::this_thread::sleep_for(std::chrono::seconds(40));
        VERIFY_IS_TRUE(static_cast<bool>(QueryDiagnostics(session).Running));

        // Releasing the process proxy drops the activity count to zero and the VM idle-terminates.
        process.reset();
        WaitForVmRunningState(session, false);
    }

    // A client may hold a proxy to a container that has exited and is therefore no longer "active"
    // by state. Tearing the VM down would disconnect that proxy (leaving the client with
    // RPC_E_DISCONNECTED), so the idle worker must keep the VM alive while any container proxy is
    // outstanding -- and reclaim it promptly once the client releases the proxy. This is the
    // container analogue of the root-process keep-alive above.
    WSLC_TEST_METHOD(WSLCE2E_VmIdle_HeldContainerProxyKeepsVmAlive)
    {
        auto session = TestSession::Create(L"wslc-vmidle-heldcontainer");

        EnsureImageIsLoaded(AlpineImage, session.Name());

        // Launch a container that exits almost immediately, then keep the returned proxy. Once it has
        // exited it no longer counts as active by state, so only the held proxy can keep the VM up.
        wsl::windows::common::WSLCContainerLauncher launcher(
            wsl::shared::string::WideToMultiByte(AlpineImage.NameAndTag()),
            "wslc-vmidle-heldcontainer",
            {"/bin/true"},
            {},
            "none");

        std::optional<wsl::windows::common::RunningWSLCContainer> container = launcher.Launch(*session.Session(), WSLCContainerStartFlagsNone);

        // Exercise the pure proxy-release path (not container deletion) as the trigger for teardown.
        container->SetDeleteOnClose(false);

        // Wait for the container to exit so it no longer keeps the VM alive by being Created/Running.
        retry::RetryWithTimeout<void>(
            [&]() { THROW_HR_IF(E_FAIL, container->State() != WslcContainerStateExited); },
            std::chrono::milliseconds(250),
            std::chrono::seconds(60));

        // The VM must remain running well past the idle grace period (30s) while the exited
        // container's proxy is held. Without the pin the idle worker would tear the VM down ~30s
        // after the launch returned, so a generous margin past the grace period catches that
        // regression reliably.
        std::this_thread::sleep_for(std::chrono::seconds(40));
        VERIFY_IS_TRUE(static_cast<bool>(QueryDiagnostics(session).Running));

        // Releasing the container proxy drops the last external reference and the VM idle-terminates.
        container.reset();
        WaitForVmRunningState(session, false);
    }

    // arrives while teardown may still be in flight. All operations must succeed (no spurious
    // ERROR_INVALID_STATE from racing a VM that is stopping).
    WSLC_TEST_METHOD(WSLCE2E_VmIdle_ConcurrentRecreateDoesNotFail)
    {
        auto session = TestSession::Create(L"wslc-vmidle-stress");

        EnsureImageIsLoaded(AlpineImage, session.Name());

        for (int i = 0; i < 12; i++)
        {
            // Intentionally do not wait between iterations so each lease races the previous
            // run's idle teardown.
            auto result = RunWslc(
                std::format(L"container run --session {} --rm {} echo iteration-{}", session.Name(), AlpineImage.NameAndTag(), i));
            result.Verify({.Stderr = L"", .ExitCode = 0});
        }
    }
};

} // namespace WSLCE2ETests
