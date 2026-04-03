/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    wslpath.c

Abstract:

    This file contains the function definitions for wslpath.

--*/

#include "common.h"
#include <sys/mount.h>
#include <sys/utsname.h>
#include <ctype.h>
#include <locale.h>
#include <stdarg.h>
#include <signal.h>
#include <endian.h>
#include <lxbusapi.h>
#include <pwd.h>
#include "wslpath.h"

#include "util.h"
#include "CommandLine.h"

using namespace wsl::shared;

#define INVALID_USAGE() Die(Argv[0], EINVAL, true, NULL)

std::string ExpandWindowsEnvironmentVariables(const char* Path);

std::string AbsolutePath(char* Path, char* Cwd, size_t CwdSize, bool* Relative);

std::string AbsoluteWindowsPath(char* RelativePath, const char* Cwd);

int CollapsePath(char* Path, char Separator);

void Die(const char* Argv0, int Error, bool PrintUsage, const char* Message, ...);

std::string DosToCanonicalPath(char* Path, char* UnixCwd, size_t UnixCwdSize, bool* Relative);

std::string AbsolutePath(char* Path, char* Cwd, size_t CwdSize, bool* Relative)

/*++

Routine Description:

    This routine converts a Unix path to an absolute path.

Arguments:

    RelativePath - Supplies a pointer to the relative path.

    Cwd - Supplies a buffer which receives the current working directory, only
        if the path is relative.

    CwdSize - Supplies the size of the Cwd buffer.

    Relative - Supplies a pointer to receive whether the input path was
        relative.

Return Value:

    The absolute path as a string.
    Returns an empty string on failure.

--*/

try
{
    std::string NewPath{};
    if (Path[0] != PATH_SEP)
    {
        if (getcwd(Cwd, CwdSize) == NULL)
        {
            return {};
        }

        NewPath += Cwd;
        NewPath += PATH_SEP;
        NewPath += Path;
        *Relative = true;
    }
    else
    {
        NewPath = Path;
        *Relative = false;
    }

    if (CollapsePath(NewPath.data(), PATH_SEP) < 0)
    {
        return {};
    }

    return NewPath;
}
catch (...)
{
    LOG_CAUGHT_EXCEPTION();
    return {};
}

std::string AbsoluteWindowsPath(char* RelativePath, const char* Cwd)

/*++

Routine Description:

    This routine converts a relative Win32 path to an absolute path.

    The input path must be relative.

Arguments:

    RelativePath - Supplies a pointer to the relative path.

    Cwd - Supplies a pointer to the Windows current working directory.

Return Value:

    0 on success, <0 on failure.

--*/

try
{
    std::string AbsolutePath{};
    if (RelativePath[0] == PATH_SEP_NT)
    {
        //
        // This path is relative to the drive letter root.
        //

        if (!isalpha(Cwd[0]) || Cwd[1] != ':')
        {
            return {};
        }

        AbsolutePath += Cwd[0];
        AbsolutePath += Cwd[1];
    }
    else
    {
        AbsolutePath += Cwd;
        AbsolutePath += PATH_SEP_NT;
    }

    AbsolutePath += RelativePath;
    return AbsolutePath;
}
catch (...)
{
    LOG_CAUGHT_EXCEPTION();
    return {};
}

int CollapsePath(char* Path, char Separator)

/*++

Routine Description:

    Collapses, where possible, relative path segments in a Path.

Arguments:

    Path - Supplies the path to collapse.

    Separator - The directory separator.

Return Value:

    0 on success, <0 on failure.

--*/

{
    char* LastSegment;
    char* NextSeparator;
    size_t PathLength;
    size_t Remaining;
    int RemoveSegmentCount;
    char* Segment;
    size_t SegmentLength;

    PathLength = strlen(Path);
    LastSegment = Path + PathLength;
    RemoveSegmentCount = 0;
    for (Remaining = PathLength; Remaining > 0;)
    {
        NextSeparator = static_cast<char*>(memrchr(Path, Separator, Remaining));
        if (NextSeparator == nullptr)
        {
            break;
        }

        Segment = NextSeparator + 1;
        SegmentLength = Remaining - (Segment - Path);
        Remaining -= SegmentLength + 1;
        if (SegmentLength == 2 && Segment[0] == '.' && Segment[1] == '.')
        {
            //
            // Eliminate this and the next segment.
            //

            RemoveSegmentCount++;
        }
        else
        {
            if (RemoveSegmentCount == 0 && (SegmentLength == 0 || (SegmentLength == 1 && Segment[0] == '.')))
            {
                RemoveSegmentCount++;
            }

            if (RemoveSegmentCount > 0)
            {
                memmove(Segment, LastSegment, Path + PathLength - LastSegment + 1);
                RemoveSegmentCount--;
            }

            LastSegment = Segment;
        }
    }

    return RemoveSegmentCount == 0 ? 0 : -EINVAL;
}

void Die(const char* Argv0, int Error, bool PrintUsage, const char* Message, ...)

/*++

Routine Description:

    This routine aborts program execution with an error message.

Arguments:

    Argv0 - Supplies the name of the program.

    Error - Supplies the error code or 0.

    PrintUsage - Supplies a boolean specifying if usage should be printed.

    Message - Supplies the message in printf format.

    ... - Additional arguments.

Return Value:

    None. Does not return.

--*/

{
    va_list Args;

    va_start(Args, Message);

    fprintf(stderr, "%s: ", Argv0);
    if (Message != nullptr)
    {
        vfprintf(stderr, Message, Args);
    }

    if (Error != 0)
    {
        if (Message != nullptr)
        {
            fputs(": ", stderr);
        }

        fputs(strerror(Error), stderr);
    }

    fputs("\n", stderr);
    if (PrintUsage)
    {
        printf("%s\n", Localization::MessageWslPathUsage().c_str());
    }

    exit(1);
    va_end(Args);
}

std::string DosToCanonicalPath(char* Path, char* UnixCwd, size_t UnixCwdSize, bool* Relative)

/*++

Routine Description:

    This routine gets the canonical representation of a DOS path:
    - The path is made absolute.
    - All separators are changed to backslashes.
    - Duplicate separators and .. components are compacted.
    - The \\?\ prefix is stripped from long paths.

Arguments:

    Path - Supplies the path to translate.

    UnixCwd - Supplies a buffer which receives the Linux current working
        directory, only if the path was relative.

    UnixCwdSize - Supplies the size of the cwd buffer.

    Relative - Supplies a pointer to receive whether the input path was
        a relative path.

Return Value:

    0 on success, -error for errors.

--*/

{
    std::string AbsolutePath{};
    size_t CollapseStartIndex;
    char* SuffixString;
    size_t StartIndex;
    size_t PathLength;
    int Result = -1;

    *Relative = false;

    //
    // Convert relative paths to absolute paths.
    //

    if (!UtilIsAbsoluteWindowsPath(Path))
    {
        if (getcwd(UnixCwd, UnixCwdSize) == NULL)
        {
            return {};
        }

        const auto Cwd = UtilWinPathTranslate(UnixCwd, false);
        if (Cwd.empty())
        {
            return {};
        }

        AbsolutePath = AbsoluteWindowsPath(Path, Cwd.c_str());
        if (AbsolutePath.empty())
        {
            return {};
        }

        Path = AbsolutePath.data();
        *Relative = true;
    }

    PathLength = strlen(Path);
    if (PathLength < 2)
    {
        return {};
    }

    if (Path[1] == DRIVE_SEP_NT && Path[2] == PATH_SEP_NT)
    {
        //
        // This is a drive letter path C:\foo -> \??\C:\foo
        //

        CollapseStartIndex = 0;
        StartIndex = 0;
    }
    else if (Path[0] == PATH_SEP_NT && Path[1] == PATH_SEP_NT)
    {
        //
        // This is either a long path or a UNC path. If it's a long path,
        // strip the prefix.
        //

        if (Path[2] == '?' && Path[3] == PATH_SEP_NT)
        {
            StartIndex = 4;
            CollapseStartIndex = 0;
        }
        else
        {
            StartIndex = 0;

            //
            // Make sure the starting \\ aren't collapsed
            //

            CollapseStartIndex = 2;
        }
    }
    else
    {
        return {};
    }

    SuffixString = Path + StartIndex;
    Result = CollapsePath(&SuffixString[CollapseStartIndex], PATH_SEP_NT);
    if (Result < 0)
    {
        return {};
    }

    return std::string(SuffixString);
}

std::string ExpandWindowsEnvironmentVariables(const char* Path)

/*++

Routine Description:

    This routine expands Windows-style environment variable references (%VAR%)
    in the given path string. Each %VAR% is resolved by querying the Windows
    environment block stored by init via UtilGetEnvironmentVariable.

    A literal percent can be represented as %% which expands to a single %.
    If a variable cannot be resolved, the %VAR% reference is left as-is.

Arguments:

    Path - Supplies the path string that may contain %VAR% references.

Return Value:

    The expanded path string if any variables were found, or an empty string
    if no expansion was needed.

--*/

try
{
    std::string Result{};
    const char* Current = Path;
    bool FoundVariable = false;

    while (*Current != '\0')
    {
        if (*Current != '%')
        {
            Result += *Current;
            Current++;
            continue;
        }

        //
        // Found a '%'. Look for the closing '%'.
        //

        const char* VarStart = Current + 1;
        const char* VarEnd = strchr(VarStart, '%');
        if (VarEnd == nullptr)
        {
            //
            // No closing '%', copy the rest of the string as-is.
            //

            Result += Current;
            break;
        }

        if (VarEnd == VarStart)
        {
            //
            // %% expands to a literal %.
            //

            Result += '%';
            Current = VarEnd + 1;
            continue;
        }

        //
        // Extract the variable name and query the Windows environment.
        //

        std::string VarName(VarStart, VarEnd - VarStart);
        auto Value = UtilGetWindowsEnvironmentVariable(VarName.c_str());
        if (!Value.empty())
        {
            Result += Value;
            FoundVariable = true;
        }
        else
        {
            //
            // Variable not found; preserve the original %VAR% text.
            //

            Result += '%';
            Result += VarName;
            Result += '%';
        }

        Current = VarEnd + 1;
    }

    return FoundVariable ? Result : std::string{};
}
catch (...)
{
    LOG_CAUGHT_EXCEPTION();
    return {};
}

int WslPathEntry(int Argc, char* Argv[])

/*++

Routine Description:

    This routine will output to stdout the NT path for a DrvFs path.

Arguments:

    Argc - Supplies the argument count.

    Argv - Supplies the command line arguments.

Return Value:

    0 on success, 1 on failure.

--*/

{
    int Flags = TRANSLATE_FLAG_RESOLVE_SYMLINKS;
    std::optional<char> Mode;
    const char* OriginalPath{};
    std::string OutputPath;
    int Result{};
    char* SourcePath = nullptr;

    //
    // With the current version of musl, this has no useful effect but is also
    // not harmful.
    //

    setlocale(LC_ALL, "");

    ArgumentParser parser(Argc, Argv);

    constexpr auto Usage = std::bind(Localization::MessageWslPathUsage, Localization::Options::Default);

    parser.AddPositionalArgument(OriginalPath, 0);
    parser.AddArgument(SetFlag<int, TRANSLATE_FLAG_ABSOLUTE>{Flags}, nullptr, TRANSLATE_MODE_ABSOLUTE);
    parser.AddArgument(UniqueSetValue<char, TRANSLATE_MODE_UNIX>{Mode, Usage}, nullptr, TRANSLATE_MODE_UNIX);
    parser.AddArgument(UniqueSetValue<char, TRANSLATE_MODE_WINDOWS>{Mode, Usage}, nullptr, TRANSLATE_MODE_WINDOWS);
    parser.AddArgument(UniqueSetValue<char, TRANSLATE_MODE_MIXED>{Mode, Usage}, nullptr, TRANSLATE_MODE_MIXED);
    parser.AddArgument(UniqueSetValue<char, TRANSLATE_MODE_HELP>{Mode, Usage}, "--help");

    try
    {
        parser.Parse();
    }
    catch (const wil::ExceptionWithUserMessage& e)
    {
        fprintf(stderr, "%s\n", e.what());
        return 1;
    }

    if (OriginalPath == nullptr || Mode == TRANSLATE_MODE_HELP)
    {
        INVALID_USAGE();
    }

    SourcePath = strdup(OriginalPath);
    if (SourcePath == nullptr)
    {
        Die(Argv[0], errno, false, nullptr);
    }

    //
    // Translate the path.
    //

    OutputPath = WslPathTranslate(SourcePath, Flags, Mode.value_or(TRANSLATE_MODE_UNIX));
    if (OutputPath.empty())
    {
        Die(Argv[0], errno, false, "%s", OriginalPath);
    }

    //
    // Print the translated path and a newline.
    //

    Result = printf("%s\n", OutputPath.empty() ? "." : OutputPath.c_str());
    if (Result < 0)
    {
        Die(Argv[0], errno, false, nullptr);
    }

    //
    // Don't bother cleaning up since the process is exiting.
    //

    return 0;
}

std::string WslPathTranslate(char* Path, int Flags, char Mode)

/*++

Routine Description:

    This routine translates an absolute or relative NT or DrvFs path.

Arguments:

    Path - Supplies the path to translate.

    Flags - Supplies flags for the operation.

    Mode - Supplies the translation mode.

Return Value:

    The translated path.

--*/

{
    bool Absolute;
    std::string CanonicalPath{};
    std::string ExpandedPath{};
    char* OutputCwd = nullptr;
    size_t OutputCwdLength;
    char* RealPath = nullptr;
    bool Relative;
    std::string TranslatedPath{};
    char UnixCwd[PATH_MAX];
    std::string WindowsCwd{};

    //
    // Validate input.
    //

    if (!Path || Flags & ~(TRANSLATE_FLAG_ABSOLUTE | TRANSLATE_FLAG_RESOLVE_SYMLINKS))
    {
        goto WslPathTranslateExit;
    }

    Absolute = ((Flags & TRANSLATE_FLAG_ABSOLUTE) != 0);

    //
    // Validate the translation mode.
    //

    switch (Mode)
    {
    case TRANSLATE_MODE_UNIX:
    case TRANSLATE_MODE_WINDOWS:
    case TRANSLATE_MODE_MIXED:
        break;

    default:
        goto WslPathTranslateExit;
    }

    //
    // Expand any Windows environment variable references (%VAR%) in the path.
    // If expansion occurs, the result is a Windows-format path.
    //

    ExpandedPath = ExpandWindowsEnvironmentVariables(Path);
    if (!ExpandedPath.empty())
    {
        if (Mode != TRANSLATE_MODE_UNIX)
        {
            //
            // The expanded result is already a Windows path. For -m mode,
            // convert backslashes to forward slashes.
            //

            if (Mode == TRANSLATE_MODE_MIXED)
            {
                UtilCanonicalisePathSeparator(ExpandedPath, PATH_SEP);
            }

            return ExpandedPath;
        }

        //
        // For -u mode, use the expanded Windows path as input for
        // Windows-to-Linux translation.
        //

        Path = ExpandedPath.data();
    }

    //
    // Get the current working directory for relative path translations.
    //

    if (getcwd(UnixCwd, sizeof(UnixCwd)) == NULL)
    {
        goto WslPathTranslateExit;
    }

    //
    // Get the canonical path.
    //

    if (Mode == TRANSLATE_MODE_UNIX)
    {
        UtilCanonicalisePathSeparator(Path, PATH_SEP_NT);
        OutputCwd = UnixCwd;
        CanonicalPath = DosToCanonicalPath(Path, UnixCwd, sizeof(UnixCwd), &Relative);
        if (CanonicalPath.empty())
        {
            goto WslPathTranslateExit;
        }
    }
    else
    {
        //
        // If the resolve symlinks flag is specified, resolve any dot entries or
        // symlinks in the path. For paths that do not exist, use the supplied path
        // as-is.
        //

        if ((Flags & TRANSLATE_FLAG_RESOLVE_SYMLINKS) != 0)
        {
            RealPath = realpath(Path, NULL);
            if (RealPath != nullptr)
            {
                //
                // Preserve trailing slashes.
                //

                std::string_view PathView = Path;
                if (!PathView.empty() && PathView.back() == PATH_SEP)
                {
                    CanonicalPath = RealPath;
                    CanonicalPath += PATH_SEP;
                    Path = CanonicalPath.data();
                }
                else
                {
                    Path = RealPath;
                }
            }
            else if (errno != ENOENT)
            {
                goto WslPathTranslateExit;
            }
        }

        CanonicalPath = AbsolutePath(Path, UnixCwd, sizeof(UnixCwd), &Relative);
        if (CanonicalPath.empty())
        {
            goto WslPathTranslateExit;
        }

        //
        // Translate the cwd if it will be needed to translate back to a
        // relative path.
        //

        if (Relative && !Absolute)
        {
            WindowsCwd = UtilWinPathTranslate(UnixCwd, false);
            OutputCwd = WindowsCwd.empty() ? NULL : WindowsCwd.data();
        }
    }

    //
    // Perform the translation
    //

    TranslatedPath = UtilWinPathTranslate(CanonicalPath.data(), Mode == TRANSLATE_MODE_UNIX);
    if (TranslatedPath.empty())
    {
        goto WslPathTranslateExit;
    }

    //
    // Convert the absolute path back into a relative one
    // if it is a subpath of the current directory.
    //

    if (Relative && !Absolute && OutputCwd != nullptr)
    {
        OutputCwdLength = strlen(OutputCwd);
        if (strncmp(TranslatedPath.c_str(), OutputCwd, OutputCwdLength) == 0)
        {
            switch (TranslatedPath[OutputCwdLength])
            {
            case PATH_SEP_NT:
            case PATH_SEP:
                if (OutputCwdLength + 1 == TranslatedPath.length())
                {
                    // Special case for wslpath -u .
                    TranslatedPath = ".";
                }
                else
                {
                    TranslatedPath = TranslatedPath.substr(OutputCwdLength + 1);
                }
                break;

            case '\0':
                TranslatedPath = TranslatedPath.substr(OutputCwdLength);
                break;
            }
        }
    }

    if (Mode == TRANSLATE_MODE_MIXED)
    {
        UtilCanonicalisePathSeparator(TranslatedPath, PATH_SEP);
    }

WslPathTranslateExit:
    free(RealPath);
    return TranslatedPath;
}
