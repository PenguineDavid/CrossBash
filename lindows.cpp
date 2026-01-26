/*
 * Lindows v2.2 - Windows Command Shell
 * Copyright © 2026 David S
 * License: polyform noncommercial 1.0.0
 * https://github.com/PenguineDavid/winux2.0?tab=License-1-ov-file
 * https://polyformproject.org/licenses/noncommercial/1.0.0/
 * A full-featured shell environment for Windows with Linux-like syntax
 * WARNING: This tool executes commands with user privileges. Use responsibly.
 */

// ----------------------
// SYSTEM INCLUDES
// ----------------------
#include <iostream>
#include <string>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <vector>
#include <cstdlib>
#include <memory>
#include <csignal>

#ifdef _WIN32
#include <windows.h>
#include <process.h>
#else
#error "This shell is designed for Windows only"
#endif

// ----------------------
// CONSTANTS
// ----------------------
constexpr size_t MAX_CMD_LENGTH = 8192;
constexpr size_t MAX_PATH_LENGTH = 32767;

// ----------------------
// GLOBAL VARIABLES FOR SIGNAL HANDLING
// ----------------------
static volatile bool g_interruptRequested = false;
static HANDLE g_currentProcessHandle = nullptr;

// ----------------------
// SIGNAL HANDLER
// ----------------------
#ifdef _WIN32
BOOL WINAPI ConsoleHandler(DWORD signal)
{
    if (signal == CTRL_C_EVENT)
    {
        g_interruptRequested = true;
        if (g_currentProcessHandle != nullptr)
        {
            // Send Ctrl+C to the running process
            GenerateConsoleCtrlEvent(0, 0); // Send to all processes in console
            TerminateProcess(g_currentProcessHandle, 1);
            g_currentProcessHandle = nullptr;
        }
        return TRUE; // We handled it
    }
    return FALSE;
}
#endif

// ----------------------
// JOB MANAGEMENT
// ----------------------
struct Job
{
    DWORD processId;
    HANDLE processHandle;
    std::string commandLine;

    ~Job()
    {
        if (processHandle && processHandle != INVALID_HANDLE_VALUE)
        {
            CloseHandle(processHandle);
        }
    }
};

std::vector<std::unique_ptr<Job>> activeJobs;

// ----------------------
// PIPE SUPPORT
// ----------------------
struct Pipe
{
    HANDLE readEnd;
    HANDLE writeEnd;

    Pipe() : readEnd(nullptr), writeEnd(nullptr) {}

    ~Pipe()
    {
        if (readEnd && readEnd != INVALID_HANDLE_VALUE)
        {
            CloseHandle(readEnd);
        }
        if (writeEnd && writeEnd != INVALID_HANDLE_VALUE)
        {
            CloseHandle(writeEnd);
        }
    }

    bool create()
    {
        SECURITY_ATTRIBUTES securityAttr = {
            sizeof(SECURITY_ATTRIBUTES),
            NULL,
            TRUE // Handles are inheritable
        };

        return CreatePipe(&readEnd, &writeEnd, &securityAttr, 0) != 0;
    }
};

// ----------------------
// UTILITY FUNCTIONS
// ----------------------
namespace Utils
{
    std::string trim(const std::string &str)
    {
        const size_t start = str.find_first_not_of(" \t");
        if (start == std::string::npos)
            return "";

        const size_t end = str.find_last_not_of(" \t");
        return str.substr(start, end - start + 1);
    }

    std::string toLower(std::string str)
    {
        std::transform(str.begin(), str.end(), str.begin(),
                       [](unsigned char c)
                       { return std::tolower(c); });
        return str;
    }

    std::string getHomeDirectory()
    {
        const char *homeDrive = std::getenv("HOMEDRIVE");
        const char *homePath = std::getenv("HOMEPATH");

        if (homeDrive && homePath)
        {
            return std::string(homeDrive) + homePath;
        }

        const char *userProfile = std::getenv("USERPROFILE");
        return userProfile ? userProfile : "C:\\";
    }

    std::string expandEnvironmentVariables(const std::string &input)
    {
        std::string result;
        result.reserve(input.size());

        for (size_t i = 0; i < input.size(); ++i)
        {
            if (input[i] == '$' && i + 1 < input.size())
            {
                size_t varStart = i + 1;
                size_t varEnd = varStart;

                while (varEnd < input.size() &&
                       (std::isalnum(input[varEnd]) || input[varEnd] == '_'))
                {
                    ++varEnd;
                }

                std::string varName = input.substr(varStart, varEnd - varStart);
                const char *varValue = std::getenv(varName.c_str());

                if (varValue)
                {
                    result += varValue;
                }
                else
                {
                    result += '$' + varName;
                }

                i = varEnd - 1;
            }
            else
            {
                result += input[i];
            }
        }

        return result;
    }

    bool containsDangerousPatterns(const std::string &cmd)
    {
        const std::string dangerous[] = {"`", "$(", "<(", ">(", ";"};

        for (const auto &pattern : dangerous)
        {
            if (cmd.find(pattern) != std::string::npos)
            {
                return true;
            }
        }

        return false;
    }

    void enableConsoleFeatures()
    {
#ifdef _WIN32
        HANDLE outputHandle = GetStdHandle(STD_OUTPUT_HANDLE);
        if (outputHandle != INVALID_HANDLE_VALUE)
        {
            DWORD consoleMode = 0;
            if (GetConsoleMode(outputHandle, &consoleMode))
            {
                consoleMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
                SetConsoleMode(outputHandle, consoleMode);
            }
        }

        // Set up Ctrl+C handler
        SetConsoleCtrlHandler(ConsoleHandler, TRUE);
#endif
    }

    // Simple parser for commands
    struct CommandSegment
    {
        std::string command;
        std::string inputFile;
        std::string outputFile;
        bool appendMode = false;
        bool background = false;
        bool hasInputRedirection = false;
        bool hasOutputRedirection = false;
    };

    std::vector<CommandSegment> parseCommandLine(const std::string &input)
    {
        std::vector<CommandSegment> segments;
        std::string currentSegment;

        bool inSingleQuote = false;
        bool inDoubleQuote = false;

        for (size_t i = 0; i < input.size(); ++i)
        {
            char c = input[i];

            if (c == '\\' && i + 1 < input.size() && !inSingleQuote)
            {
                currentSegment += input[++i];
                continue;
            }

            if (c == '\'' && !inDoubleQuote)
            {
                inSingleQuote = !inSingleQuote;
            }
            else if (c == '"' && !inSingleQuote)
            {
                inDoubleQuote = !inDoubleQuote;
            }
            else if (c == '|' && !inSingleQuote && !inDoubleQuote)
            {
                segments.push_back(CommandSegment{trim(currentSegment)});
                currentSegment.clear();
                continue;
            }

            currentSegment += c;
        }

        if (!currentSegment.empty())
        {
            segments.push_back(CommandSegment{trim(currentSegment)});
        }

        // Parse each segment for redirections
        for (auto &segment : segments)
        {
            std::string &cmd = segment.command;

            // Check for background
            if (!cmd.empty() && cmd.back() == '&')
            {
                segment.background = true;
                cmd.pop_back();
                cmd = trim(cmd);
            }

            // Simple redirection parsing
            // Check for output redirection >
            size_t pos = cmd.find('>');
            if (pos != std::string::npos)
            {
                // Check if it's append mode >>
                if (pos + 1 < cmd.size() && cmd[pos + 1] == '>')
                {
                    segment.hasOutputRedirection = true;
                    segment.appendMode = true;
                    segment.outputFile = trim(cmd.substr(pos + 2));
                    cmd = trim(cmd.substr(0, pos));
                }
                else
                {
                    segment.hasOutputRedirection = true;
                    segment.outputFile = trim(cmd.substr(pos + 1));
                    cmd = trim(cmd.substr(0, pos));
                }
            }

            // Check for input redirection <
            pos = cmd.find('<');
            if (pos != std::string::npos)
            {
                segment.hasInputRedirection = true;
                segment.inputFile = trim(cmd.substr(pos + 1));
                cmd = trim(cmd.substr(0, pos));
            }

            cmd = trim(cmd);
        }

        return segments;
    }
}

// ----------------------
// PROMPT MANAGER
// ----------------------
class PromptManager
{
private:
    std::string username;
    std::string hostname;

    bool fetchUsername()
    {
        char buffer[256];
        DWORD bufferSize = sizeof(buffer);

        if (GetUserNameA(buffer, &bufferSize))
        {
            username = buffer;
            return true;
        }

        username = "user";
        return false;
    }

    bool fetchHostname()
    {
        char buffer[256];
        DWORD bufferSize = sizeof(buffer);

        // Use DNS hostname instead of NetBIOS name
        if (GetComputerNameExA(ComputerNameDnsHostname, buffer, &bufferSize))
        {
            hostname = buffer;

            // Optional: normalize to Linux style
            // lowercase everything, capitalize first letter
            std::transform(hostname.begin(), hostname.end(), hostname.begin(),
                           [](unsigned char c)
                           { return std::tolower(c); });

            if (!hostname.empty())
                hostname[0] = std::toupper(hostname[0]);

            return true;
        }

        hostname = "localhost";
        return false;
    }

    std::string formatPath(const std::filesystem::path &path) const
    {
        std::string pathStr = path.string();
        std::string homeDir = Utils::getHomeDirectory();

        if (!homeDir.empty() &&
            pathStr.size() >= homeDir.size() &&
            pathStr.compare(0, homeDir.size(), homeDir) == 0)
        {
            pathStr.replace(0, homeDir.size(), "~");
        }

        std::replace(pathStr.begin(), pathStr.end(), '\\', '/');
        return pathStr;
    }

public:
    PromptManager()
    {
        fetchUsername();
        fetchHostname();
    }

    std::string generate() const
    {
        const std::string green = "\033[32m";
        const std::string blue = "\033[34m";
        const std::string reset = "\033[0m";

        std::string currentDir;
        try
        {
            currentDir = formatPath(std::filesystem::current_path());
        }
        catch (const std::filesystem::filesystem_error &)
        {
            currentDir = "?";
        }

        return green + username + "@" + hostname + reset + ":" +
               blue + currentDir + reset + "$ ";
    }
};

// ----------------------
// BUILT-IN COMMANDS
// ----------------------
namespace Builtins
{
    void changeDirectory(const std::string &path)
    {
        std::filesystem::path targetPath;

        if (path.empty() || path == "~")
        {
            targetPath = Utils::getHomeDirectory();
        }
        else
        {
            targetPath = Utils::expandEnvironmentVariables(path);

            if (!targetPath.is_absolute())
            {
                targetPath = std::filesystem::absolute(targetPath);
            }
        }

        std::error_code error;
        std::filesystem::current_path(targetPath, error);

        if (error)
        {
            std::cerr << "cd: Cannot access '" << path << "': "
                      << error.message() << std::endl;
        }
    }

    void printWorkingDirectory()
    {
        try
        {
            std::cout << std::filesystem::current_path().string() << std::endl;
        }
        catch (const std::filesystem::filesystem_error &e)
        {
            std::cerr << "pwd: " << e.what() << std::endl;
        }
    }

    void listDirectory(const std::string &args = "")
    {
        try
        {
            std::string path = args.empty() ? std::filesystem::current_path().string() : Utils::expandEnvironmentVariables(args);

            for (const auto &entry : std::filesystem::directory_iterator(path))
            {
                std::cout << entry.path().filename().string() << "  ";
            }
            std::cout << std::endl;
        }
        catch (const std::filesystem::filesystem_error &e)
        {
            std::cerr << "ls: " << e.what() << std::endl;
        }
    }

    void createDirectory(const std::string &path)
    {
        if (path.empty())
        {
            std::cerr << "mkdir: Missing operand" << std::endl;
            return;
        }

        std::error_code error;
        if (!std::filesystem::create_directory(path, error))
        {
            std::cerr << "mkdir: Failed to create '" << path << "': "
                      << error.message() << std::endl;
        }
    }

    void removeDirectory(const std::string &path)
    {
        if (path.empty())
        {
            std::cerr << "rmdir: Missing operand" << std::endl;
            return;
        }

        std::error_code error;
        if (!std::filesystem::remove(path, error))
        {
            std::cerr << "rmdir: Failed to remove '" << path << "': "
                      << error.message() << std::endl;
        }
    }

    void removeFile(const std::string &path, bool recursive)
    {
        if (path.empty())
        {
            std::cerr << "rm: Missing operand" << std::endl;
            return;
        }

        std::error_code error;
        bool success = recursive ? std::filesystem::remove_all(path, error) : std::filesystem::remove(path, error);

        if (!success)
        {
            std::cerr << "rm: Failed to remove '" << path << "': "
                      << error.message() << std::endl;
        }
    }

    void listJobs()
    {
        for (auto it = activeJobs.begin(); it != activeJobs.end();)
        {
            DWORD exitCode = 0;
            if (GetExitCodeProcess((*it)->processHandle, &exitCode) &&
                exitCode != STILL_ACTIVE)
            {
                it = activeJobs.erase(it);
            }
            else
            {
                std::cout << "[" << (it - activeJobs.begin() + 1) << "] PID "
                          << (*it)->processId << ": "
                          << (*it)->commandLine << std::endl;
                ++it;
            }
        }
    }

    void clearScreen()
    {
#ifdef _WIN32
        system("cls");
#endif
    }

    void showHelp()
    {
        std::cout << "Lindows v2.2 - Windows Shell Environment\n"
                  << "========================================\n"
                  << "Basic Commands:\n"
                  << "  cd [dir]       Change directory\n"
                  << "  pwd            Print working directory\n"
                  << "  ls [dir]       List directory contents\n"
                  << "  mkdir <dir>    Create directory\n"
                  << "  rmdir <dir>    Remove directory\n"
                  << "  rm [-r] <file> Remove file/directory\n"
                  << "  clear          Clear screen\n"
                  << "  jobs           List background jobs\n"
                  << "  help           Show this help\n"
                  << "  exit           Exit shell\n\n"
                  << "Advanced Features:\n"
                  << "  cmd1 | cmd2    Pipe output of cmd1 to input of cmd2\n"
                  << "  cmd &          Run command in background\n"
                  << "  cmd > file     Redirect output to file (overwrite)\n"
                  << "  cmd >> file    Redirect output to file (append)\n"
                  << "  cmd < file     Redirect input from file\n\n"
                  << "Ctrl+C: Interrupts running command, not the shell\n"
                  << "================================================\n";
    }
}

// ----------------------
// COMMAND PROCESSOR
// ----------------------
class CommandProcessor
{
private:
    static bool executeBuiltin(const Utils::CommandSegment &segment,
                               std::streambuf *originalOutput = nullptr,
                               std::streambuf *originalInput = nullptr)
    {
        const std::string &cmd = segment.command;

        if (cmd == "pwd")
        {
            Builtins::printWorkingDirectory();
            return true;
        }
        else if (cmd.rfind("ls", 0) == 0)
        {
            Builtins::listDirectory(Utils::trim(cmd.substr(2)));
            return true;
        }
        else if (cmd == "jobs")
        {
            Builtins::listJobs();
            return true;
        }
        else if (cmd == "help")
        {
            Builtins::showHelp();
            return true;
        }
        else if (cmd == "clear" || cmd == "cls")
        {
            Builtins::clearScreen();
            return true;
        }
        else if (cmd.rfind("cd", 0) == 0)
        {
            Builtins::changeDirectory(Utils::trim(cmd.substr(2)));
            return true;
        }
        else if (cmd.rfind("mkdir", 0) == 0)
        {
            Builtins::createDirectory(Utils::trim(cmd.substr(5)));
            return true;
        }
        else if (cmd.rfind("rmdir", 0) == 0)
        {
            Builtins::removeDirectory(Utils::trim(cmd.substr(5)));
            return true;
        }
        else if (cmd.rfind("rm", 0) == 0)
        {
            std::string args = Utils::trim(cmd.substr(2));
            bool recursive = (args.find("-r") != std::string::npos ||
                              args.find("-R") != std::string::npos);

            if (recursive)
            {
                size_t pos = args.find("-r");
                if (pos == std::string::npos)
                    pos = args.find("-R");
                if (pos != std::string::npos)
                {
                    args.erase(pos, 2);
                    args = Utils::trim(args);
                }
            }

            Builtins::removeFile(args, recursive);
            return true;
        }

        return false;
    }

    static bool executeSingleCommand(const Utils::CommandSegment &segment)
    {
#ifdef _WIN32
        std::string systemDir(MAX_PATH_LENGTH, '\0');
        GetSystemDirectoryA(systemDir.data(), MAX_PATH_LENGTH);
        systemDir.erase(systemDir.find('\0'));

        std::string fullCmd = "\"" + systemDir + "\\cmd.exe\" /c \"" +
                              Utils::expandEnvironmentVariables(segment.command) + "\"";

        STARTUPINFOA startupInfo = {sizeof(startupInfo)};
        PROCESS_INFORMATION processInfo = {0};

        // Handle input redirection
        HANDLE inputFile = INVALID_HANDLE_VALUE;
        if (segment.hasInputRedirection)
        {
            inputFile = CreateFileA(
                segment.inputFile.c_str(),
                GENERIC_READ,
                FILE_SHARE_READ,
                NULL,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                NULL);

            if (inputFile != INVALID_HANDLE_VALUE)
            {
                startupInfo.dwFlags |= STARTF_USESTDHANDLES;
                startupInfo.hStdInput = inputFile;
            }
        }

        // Handle output redirection
        HANDLE outputFile = INVALID_HANDLE_VALUE;
        if (segment.hasOutputRedirection)
        {
            outputFile = CreateFileA(
                segment.outputFile.c_str(),
                segment.appendMode ? FILE_APPEND_DATA : GENERIC_WRITE,
                FILE_SHARE_READ,
                NULL,
                segment.appendMode ? OPEN_ALWAYS : CREATE_ALWAYS,
                FILE_ATTRIBUTE_NORMAL,
                NULL);

            if (outputFile != INVALID_HANDLE_VALUE)
            {
                startupInfo.dwFlags |= STARTF_USESTDHANDLES;
                startupInfo.hStdOutput = outputFile;
                startupInfo.hStdError = outputFile;
            }
        }

        // Create the process WITHOUT the CREATE_NEW_CONSOLE flag
        // This ensures Ctrl+C is properly handled
        DWORD creationFlags = 0; // No CREATE_NEW_CONSOLE

        BOOL success = CreateProcessA(
            NULL,
            fullCmd.data(),
            NULL,
            NULL,
            TRUE, // Inherit handles
            creationFlags,
            NULL,
            NULL,
            &startupInfo,
            &processInfo);

        // Store the process handle for Ctrl+C handling
        if (!segment.background)
        {
            g_currentProcessHandle = processInfo.hProcess;
        }

        // Close file handles
        if (inputFile != INVALID_HANDLE_VALUE)
            CloseHandle(inputFile);
        if (outputFile != INVALID_HANDLE_VALUE)
            CloseHandle(outputFile);

        if (!success)
        {
            DWORD error = GetLastError();
            std::cerr << "Command failed (Error " << error << "): "
                      << segment.command << std::endl;
            return false;
        }

        if (segment.background)
        {
            auto job = std::make_unique<Job>();
            job->processId = processInfo.dwProcessId;
            job->processHandle = processInfo.hProcess;
            job->commandLine = segment.command;
            activeJobs.push_back(std::move(job));

            CloseHandle(processInfo.hThread);
            std::cout << "[" << activeJobs.size() << "] "
                      << processInfo.dwProcessId << std::endl;
        }
        else
        {
            // Wait for process with ability to handle Ctrl+C
            DWORD waitResult = WaitForSingleObject(processInfo.hProcess, INFINITE);

            // Check if we were interrupted
            if (g_interruptRequested)
            {
                std::cout << "^C" << std::endl;
                g_interruptRequested = false;
            }

            CloseHandle(processInfo.hProcess);
            CloseHandle(processInfo.hThread);
        }

        // Clear the current process handle
        if (!segment.background)
        {
            g_currentProcessHandle = nullptr;
        }

        return true;
#else
        return false;
#endif
    }

    static bool executePipedCommands(const std::vector<Utils::CommandSegment> &segments)
    {
#ifdef _WIN32
        size_t numCommands = segments.size();
        if (numCommands == 0)
            return true;

        // Create pipes between commands
        std::vector<std::unique_ptr<Pipe>> pipes;
        for (size_t i = 0; i < numCommands - 1; ++i)
        {
            auto pipe = std::make_unique<Pipe>();
            if (!pipe->create())
            {
                std::cerr << "Error: Failed to create pipe\n";
                return false;
            }
            pipes.push_back(std::move(pipe));
        }

        std::vector<PROCESS_INFORMATION> processInfos(numCommands);
        std::vector<HANDLE> fileHandles; // For input/output files

        // Store all process handles for Ctrl+C
        std::vector<HANDLE> foregroundProcessHandles;

        for (size_t i = 0; i < numCommands; ++i)
        {
            const auto &segment = segments[i];

            std::string systemDir(MAX_PATH_LENGTH, '\0');
            GetSystemDirectoryA(systemDir.data(), MAX_PATH_LENGTH);
            systemDir.erase(systemDir.find('\0'));

            std::string fullCmd = "\"" + systemDir + "\\cmd.exe\" /c \"" +
                                  Utils::expandEnvironmentVariables(segment.command) + "\"";

            STARTUPINFOA startupInfo = {sizeof(startupInfo)};
            startupInfo.dwFlags = STARTF_USESTDHANDLES;

            // Set up input handle
            if (i == 0)
            {
                // First command: check for input redirection
                if (segment.hasInputRedirection)
                {
                    HANDLE inputFile = CreateFileA(
                        segment.inputFile.c_str(),
                        GENERIC_READ,
                        FILE_SHARE_READ,
                        NULL,
                        OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL,
                        NULL);
                    if (inputFile != INVALID_HANDLE_VALUE)
                    {
                        startupInfo.hStdInput = inputFile;
                        fileHandles.push_back(inputFile);
                    }
                    else
                    {
                        startupInfo.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
                    }
                }
                else
                {
                    startupInfo.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
                }
            }
            else
            {
                // Middle command: input from previous pipe
                startupInfo.hStdInput = pipes[i - 1]->readEnd;
            }

            // Set up output handle
            if (i == numCommands - 1)
            {
                // Last command: check for output redirection
                if (segment.hasOutputRedirection)
                {
                    HANDLE outputFile = CreateFileA(
                        segment.outputFile.c_str(),
                        segment.appendMode ? FILE_APPEND_DATA : GENERIC_WRITE,
                        FILE_SHARE_READ,
                        NULL,
                        segment.appendMode ? OPEN_ALWAYS : CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL,
                        NULL);
                    if (outputFile != INVALID_HANDLE_VALUE)
                    {
                        startupInfo.hStdOutput = outputFile;
                        startupInfo.hStdError = outputFile;
                        fileHandles.push_back(outputFile);
                    }
                    else
                    {
                        startupInfo.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
                        startupInfo.hStdError = GetStdHandle(STD_ERROR_HANDLE);
                    }
                }
                else
                {
                    // Output to stdout
                    startupInfo.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
                    startupInfo.hStdError = GetStdHandle(STD_ERROR_HANDLE);
                }
            }
            else
            {
                // Middle command: output to next pipe
                startupInfo.hStdOutput = pipes[i]->writeEnd;
                startupInfo.hStdError = pipes[i]->writeEnd;
            }

            // Create the process
            BOOL success = CreateProcessA(
                NULL,
                fullCmd.data(),
                NULL,
                NULL,
                TRUE, // Inherit handles
                0,
                NULL,
                NULL,
                &startupInfo,
                &processInfos[i]);

            if (!success)
            {
                DWORD error = GetLastError();
                std::cerr << "Command failed (Error " << error << "): "
                          << segment.command << std::endl;

                // Clean up created processes
                for (size_t j = 0; j < i; ++j)
                {
                    TerminateProcess(processInfos[j].hProcess, 1);
                    CloseHandle(processInfos[j].hProcess);
                    CloseHandle(processInfos[j].hThread);
                }

                // Close file handles
                for (auto handle : fileHandles)
                {
                    CloseHandle(handle);
                }

                // Close pipe ends
                for (auto &pipe : pipes)
                {
                    CloseHandle(pipe->readEnd);
                    CloseHandle(pipe->writeEnd);
                }

                return false;
            }

            // Store foreground process handles for Ctrl+C
            if (!segments[i].background)
            {
                foregroundProcessHandles.push_back(processInfos[i].hProcess);
            }

            // Close pipe ends that are no longer needed in parent
            if (i > 0)
            {
                CloseHandle(pipes[i - 1]->readEnd);
                pipes[i - 1]->readEnd = nullptr;
            }
            if (i < numCommands - 1)
            {
                CloseHandle(pipes[i]->writeEnd);
                pipes[i]->writeEnd = nullptr;
            }
        }

        // Set current process handle for Ctrl+C (first foreground process)
        if (!foregroundProcessHandles.empty())
        {
            g_currentProcessHandle = foregroundProcessHandles[0];
        }

        // Wait for all processes to complete
        for (size_t i = 0; i < numCommands; ++i)
        {
            if (segments[i].background)
            {
                // For background processes in pipe chain
                auto job = std::make_unique<Job>();
                job->processId = processInfos[i].dwProcessId;
                job->processHandle = processInfos[i].hProcess;
                job->commandLine = segments[i].command;
                activeJobs.push_back(std::move(job));
                CloseHandle(processInfos[i].hThread);
            }
            else
            {
                WaitForSingleObject(processInfos[i].hProcess, INFINITE);
                CloseHandle(processInfos[i].hProcess);
                CloseHandle(processInfos[i].hThread);
            }
        }

        // Clear current process handle
        g_currentProcessHandle = nullptr;

        // Close any remaining file handles
        for (auto handle : fileHandles)
        {
            CloseHandle(handle);
        }

        return true;
#else
        return false;
#endif
    }

public:
    static void process(const std::string &input)
    {
        if (input.empty())
            return;

        std::string trimmed = Utils::trim(input);
        if (trimmed.empty())
            return;

        if (Utils::containsDangerousPatterns(input))
        {
            std::cerr << "Warning: Command contains potentially unsafe characters\n";
            return;
        }

        // Parse command line into segments
        auto segments = Utils::parseCommandLine(input);

        if (segments.empty())
            return;

        // Single command or pipe chain
        if (segments.size() == 1)
        {
            // Handle output redirection for builtins
            std::streambuf *originalOutput = nullptr;
            std::ofstream outputStream;

            if (segments[0].hasOutputRedirection)
            {
                originalOutput = std::cout.rdbuf();
                outputStream.open(segments[0].outputFile,
                                  segments[0].appendMode ? std::ios::app : std::ios::trunc);

                if (outputStream.is_open())
                {
                    std::cout.rdbuf(outputStream.rdbuf());
                }
            }

            // Handle input redirection for builtins (simple file reading)
            std::streambuf *originalInput = nullptr;
            std::ifstream inputStream;

            if (segments[0].hasInputRedirection)
            {
                originalInput = std::cin.rdbuf();
                inputStream.open(segments[0].inputFile);

                if (inputStream.is_open())
                {
                    std::cin.rdbuf(inputStream.rdbuf());
                }
            }

            bool handled = executeBuiltin(segments[0], originalOutput, originalInput);

            if (!handled)
            {
                executeSingleCommand(segments[0]);
            }

            // Restore original buffers
            if (originalOutput)
            {
                std::cout.rdbuf(originalOutput);
            }
            if (originalInput)
            {
                std::cin.rdbuf(originalInput);
            }
        }
        else
        {
            // Pipe chain
            executePipedCommands(segments);
        }
    }
};

// ----------------------
// MAIN APPLICATION
// ----------------------
int main()
{
    std::cout << "Lindows v2.2 - Windows Shell Environment\n"
              << "========================================\n"
              << "Type 'help' for available commands, 'exit' to quit\n"
              << "Ctrl+C: Interrupts running command, or the shell when nothing is running\n"
              << "Copyright © 2026 David S \n"
              << "License: polyform noncommercial 1.0.0\n"
              << "https://github.com/PenguineDavid/winux2.0?tab=License-1-ov-file\n"
              << "https://polyformproject.org/licenses/noncommercial/1.0.0/\n"
              << "\n========================================\n\n";

    Utils::enableConsoleFeatures();
    PromptManager prompt;
    std::string userInput;

    while (true)
    {
        try
        {
            // Reset interrupt flag
            g_interruptRequested = false;

            // Clean up finished jobs
            for (auto it = activeJobs.begin(); it != activeJobs.end();)
            {
                DWORD exitCode = 0;
                if (GetExitCodeProcess((*it)->processHandle, &exitCode) &&
                    exitCode != STILL_ACTIVE)
                {
                    it = activeJobs.erase(it);
                }
                else
                {
                    ++it;
                }
            }

            std::cout << prompt.generate();

            // Check if Ctrl+C was pressed
            if (g_interruptRequested)
            {
                g_interruptRequested = false;
                std::cout << "^C\n";
                continue;
            }

            if (!std::getline(std::cin, userInput))
            {
                // Check for Ctrl+C on empty input
                if (g_interruptRequested)
                {
                    g_interruptRequested = false;
                    std::cout << "^C\n";
                    continue;
                }
                break;
            }

            if (Utils::toLower(Utils::trim(userInput)) == "exit")
            {
                break;
            }

            CommandProcessor::process(userInput);
        }
        catch (const std::exception &e)
        {
            std::cerr << "Error: " << e.what() << std::endl;
        }
    }

    std::cout << "\nGoodbye!\n";
    return 0;
}