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
        RunWslcAndVerify(
            std::format(L"container rm --session {} {}", session.Name(), containerName), {.Stderr = L"", .ExitCode = 0});

        WaitForVmRunningState(session, false);
    }

    // Stress the lease-vs-idle-teardown race: each run idles the VM, and the next run's lease
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
