"""WinePS4 v00.20: the desktop starts every program as a process of its own (no more restarting the
app into one), shows the ones running and how each ended. One-time edit of shell/desktop.cpp.
  python scripts/desktop-v20.py shell/desktop.cpp"""
import sys

path = sys.argv[1]
s = open(path, encoding='utf-8').read()


def rep(old, new, count=1):
    global s
    n = s.count(old)
    assert n == count, 'found %d of %r' % (n, old[:80])
    s = s.replace(old, new)


# the programs: running ones (pid -> name), the last ended
rep('''std::string program_status;
std::string program_name;
bool program_running = false;
bool program_done = false;
int program_code = 0;
std::atomic<bool> program_changed{false};''', '''std::string program_status;
std::map<int, std::string> programs_running;  // pid -> name
std::string last_ended;                        // "<name> ended (exit code N)"
int last_ended_code = 0;
std::deque<std::string> ended_toasts;          // for the desktop's thread
std::atomic<bool> program_changed{false};''')

rep('''    show_message("Run " + base_name(path) + "?", "WinePS4 restarts and runs it. A running program is closed first.");''',
    '''    show_message("Run " + base_name(path) + "?", "It runs next to the desktop and any other program. What it prints is in Terminal.");''')

start = s.index('void run_program(const std::string &path) {')
end = s.index('\n}\n', start) + 3
s = s[:start] + r'''// Starts a program as a process of its own (port/ps4_main.c); "" when it started, else why not
std::string launch(const std::vector<std::string> &args) {
    std::vector<char *> argv;
    for (const std::string &a : args)
        argv.push_back(const_cast<char *>(a.c_str()));
    argv.push_back(nullptr);
    const int pid = ps4_launch_program(argv.data());
    if (pid < 0)
        return errno == EAGAIN ? "Windows is still being set up: try again in a moment." : std::string("It could not start: ") + std::strerror(errno);
    {
        std::lock_guard lock(program_lock);
        programs_running[pid] = base_name(args.back() == "" ? args.front() : args[args.size() > 2 && args[0].find("msiexec") != std::string::npos ? 2 : 0]);
    }
    program_changed = true;
    return {};
}

void run_program(const std::string &path) {
    std::vector<std::string> args;
    if (ends_with(path, ".msi")) {
        args.push_back("C:\\windows\\system32\\msiexec.exe");
        args.push_back("/i");
    }
    args.push_back(path);
    std::vector<std::string> recent = read_lines(RECENT_FILE), out{path};
    for (const std::string &r : recent)
        if (r != path && out.size() < 8)
            out.push_back(r);
    write_lines(RECENT_FILE, out);
    const std::string error = launch(args);
    if (!error.empty()) {
        show_message("Run " + base_name(path), error);
        return;
    }
    toast = Toast{"Starting " + base_name(path), now_seconds() + 3};
    if (Window *w = open_window(Kind::Terminal))
        w->scroll = 0;
}
''' + s[end:]

# the Terminal's head: the programs running, else the last one's end
rep('''    if (program_name.empty()) {
        head = "No program is running. Open one from Files or the Start menu.";
    } else if (program_done) {
        head = program_name + " ended (exit code " + std::to_string(program_code) + ")";
        head_col = program_code == 0 ? rgb(0x6ccb5f) : rgb(0xff99a4);
    } else {
        head = program_name + (program_status.empty() ? " is running" : " - " + program_status);
        head_col = rgb(0x4cc2ff);
    }''', '''    if (!program_status.empty()) {
        head = "Windows: " + program_status;
        head_col = rgb(0x4cc2ff);
    } else if (!programs_running.empty()) {
        head = "Running:";
        for (const auto &[pid, name] : programs_running)
            head += " " + name + " (" + std::to_string(pid) + ")";
        head_col = rgb(0x4cc2ff);
    } else if (!last_ended.empty()) {
        head = last_ended;
        head_col = last_ended_code == 0 ? rgb(0x6ccb5f) : rgb(0xff99a4);
    } else {
        head = "No program is running. Open one from Files or the Start menu.";
    }''')

rep('''extern "C" void ps4_shell_program_ended(int status) {
    {
        std::lock_guard lock(program_lock);
        program_done = true;
        program_running = false;
        program_code = status;
    }
    program_changed = true;
}''', '''extern "C" void ps4_shell_program_exited(int pid, int code) {
    {
        std::lock_guard lock(program_lock);
        auto it = programs_running.find(pid);
        if (it == programs_running.end())
            return;  // (not one the desktop started)
        last_ended = it->second + " ended (exit code " + std::to_string(code) + ")";
        last_ended_code = code;
        ended_toasts.push_back(last_ended);
        programs_running.erase(it);
    }
    program_changed = true;
}

// launch.txt, run.txt (from the port's setup thread): started on the desktop's thread
std::mutex launch_lock;
std::vector<std::vector<std::string>> launch_queue;

extern "C" void ps4_shell_launch(char **args) {
    std::vector<std::string> a;
    for (int i = 0; args[i]; i++)
        a.push_back(args[i]);
    std::lock_guard lock(launch_lock);
    launch_queue.push_back(a);
}''')

rep('''extern "C" void ps4_shell_run(const char *program) {
    if (program) {
        std::lock_guard lock(program_lock);
        program_name = base_name(program);
        program_running = true;
    }
    block_wine_signals();
    if (!screen_open()) {
        // no screen: the program alone, as before the desktop
        for (;;) {
            sleep(1);
            std::lock_guard lock(program_lock);
            if (program_done || !program)
                exit(program_done ? program_code : 0);
        }
    }''', '''extern "C" void ps4_shell_run(const char *) {
    block_wine_signals();
    if (!screen_open()) {
        // no screen: nothing to show; programs from run.txt still run
        for (;;) {
            sleep(1);
            std::vector<std::vector<std::string>> queue;
            {
                std::lock_guard lock(launch_lock);
                queue.swap(launch_queue);
            }
            for (const auto &a : queue)
                launch(a);
        }
    }''')
rep('''    Canvas scene(W, H);
    if (program) {
        Window *w = open_window(Kind::Terminal);
        w->title = std::string("Terminal - ") + base_name(program);
    }
    bool dirty = true;
    bool ended_seen = false;''', '''    Canvas scene(W, H);
    bool dirty = true;''')
rep('''        if (program_changed.exchange(false)) {
            dirty = true;
            std::lock_guard lock(program_lock);
            if (program_done && !ended_seen) {
                ended_seen = true;
                toast = Toast{program_name + " ended (exit code " + std::to_string(program_code) + ")", t + 5};
            }
        }''', '''        if (program_changed.exchange(false)) {
            dirty = true;
            std::lock_guard lock(program_lock);
            if (!ended_toasts.empty()) {
                toast = Toast{ended_toasts.back(), t + 5};
                ended_toasts.clear();
            }
        }
        {
            std::vector<std::vector<std::string>> queue;
            {
                std::lock_guard lock(launch_lock);
                if (ps4_wine_ready())
                    queue.swap(launch_queue);
            }
            for (const auto &a : queue) {
                const std::string error = launch(a);
                if (!error.empty())
                    show_message("Run " + base_name(a.back()), error);
                dirty = true;
            }
        }''')
open(path, 'w', encoding='utf-8', newline='\n').write(s)
print('ok')
