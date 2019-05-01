#include <iostream>
#include <string>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <cstring>
#include <cassert>
#include <vector>

using std::cerr;
using std::cout;
using std::endl;
using std::string;

enum class SizeMode {
    NONE,
    LESS,
    EQUAL,
    GREATER
};

struct linux_dirent64 {
    ino64_t d_ino;
    off64_t d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[];
};

const int BUFFER_SIZE = 1024;

ino64_t inode_target;
string name_target;
std::vector<string> results;
SizeMode size_mode;
off_t size_target;
nlink_t nlinks_target;
string exec_target;

void print_error() {
    assert(errno != 0);
    cout << strerror(errno) << endl;
}

// returns false if reading file info failed
bool matches(linux_dirent64* entry, int dir_fd, string const& dir_path) {
    if (inode_target != 0 &&
        entry->d_ino != inode_target) {
            return false;
    }

    if (!name_target.empty() &&
        std::string(entry->d_name) != name_target) {
        return false;
    }

    // do not call stat if we don't have to
    if (size_mode == SizeMode::NONE && nlinks_target == 0) {
        return true;
    }

    int fd = openat(dir_fd, entry->d_name, O_RDONLY);
    if (fd == -1) {
        cout << "Error opening file at " << dir_path << entry->d_name << endl;
        print_error();
        return false;
    }

    struct stat stats{};
    int result = fstat(fd, &stats);
    if (result == -1) {
        cout << "Error reading stats of file at " << dir_path << entry->d_name << endl;
        print_error();
        return false;
    }

    if (size_mode != SizeMode::NONE) {
        switch (size_mode) {
            case SizeMode::LESS:
                if (stats.st_size > size_target) { return false; }
                break;
            case SizeMode::EQUAL:
                if (stats.st_size != size_target) { return false; }
                break;
            case SizeMode::GREATER:
                if (stats.st_size < size_target) { return false; }
                break;
            case SizeMode::NONE:
                assert(false);
        }
    }

    if (nlinks_target != 0 &&
        nlinks_target != stats.st_nlink) {
        return false;
    }

    return true;
}

void visit(int dir_fd, string const& path) {
    char buf[BUFFER_SIZE];

    while (true) {
        long read = syscall(SYS_getdents64, dir_fd, buf, BUFFER_SIZE);

        if (read == -1) {
            cout << "Error reading contents of " << path << endl;
            print_error();
            return;
        }
        if (read == 0) {
            return;
        }

        for (char* ptr = buf; ptr < buf + read;) {
            auto entry = reinterpret_cast<linux_dirent64*>(ptr);
            auto name = std::string(entry->d_name);

            if (name == "." || name == "..") {
                ptr += entry->d_reclen;
                continue;
            }

            if (entry->d_type == DT_REG && matches(entry, dir_fd, path)) {
                results.push_back(path + entry->d_name);
            } else if (entry->d_type == DT_DIR) {
                int fd = openat(dir_fd, entry->d_name, O_RDONLY | O_DIRECTORY);
                if (fd == -1) {
                    cout << "Error reading contents of " << path  << name << "/" << endl;
                    print_error();
                } else {
                    visit(fd, path + name + "/");
                    close(fd);
                }
            }
            ptr += entry->d_reclen;
        }
    }
}

void error_multiple_specified(const string &s) {
    cout << "Only one " << s << " can be specified" << endl;
}

int set_args(int argc, char* argv[]) {
    bool hasDir = false;
    int dirPosition = 0;

    for (int i = 1; i < argc;) {
        if (argv[i][0] == '-') {
            if (argc < i + 2) {
                cout << "Option " << argv[i] << " is missing its value";
                return -1;
            }

            auto option = string(argv[i]);
            if (option == "-inum") {
                if (inode_target != 0) {
                    error_multiple_specified("inode number");
                    return -1;
                }
                try {
                    inode_target = std::stoul(argv[i + 1]);
                } catch (std::logic_error& error) {
                    cout << "Bad -inum argument" << endl;
                    return -1;
                }
            } else if (option == "-name") {
                if (!name_target.empty()) {
                    error_multiple_specified("file name");
                    return -1;
                }
                name_target = argv[i + 1];
            } else if (option == "-size") {
                if (size_mode != SizeMode::NONE) {
                    error_multiple_specified("file size");
                    return -1;
                }
                bool mode_specified = true;
                switch (argv[i+1][0]) {
                    case '-':
                        size_mode = SizeMode::LESS;
                        break;
                    case '=':
                        size_mode = SizeMode::EQUAL;
                        break;
                    case '+':
                        size_mode = SizeMode::GREATER;
                        break;
                    default:
                        mode_specified = false;
                        break;
                }

                try {
                    size_target = std::stoi(argv[i+1] + (mode_specified ? 1 : 0));
                } catch (std::logic_error& error) {
                    cout << "Bad -size argument" << endl;
                    return -1;
                }
            } else if (option == "-nlinks") {
                if (nlinks_target != 0) {
                    error_multiple_specified("hardlinks number");
                    return -1;
                }
                try {
                    nlinks_target = std::stoul(argv[i + 1]);
                } catch (std::logic_error& error) {
                    cout << "Bad -nlinks argument" << endl;
                    return -1;
                }
            } else if (option == "-exec") {
                if (!exec_target.empty()) {
                    error_multiple_specified("execution target");
                    return -1;
                }
                exec_target = argv[i + 1];
            } else {
                cout << "Unknown option used: " << option << endl;
                return -1;
            }
            i+= 2;
        } else {
            if (hasDir) {
                error_multiple_specified("directory");
                return -1;
            }
            hasDir = true;
            dirPosition = i;
            i++;
        }
    }

    if (!hasDir) {
        cout << "Usage: os-find [OPTIONS] DIRECTORY" << endl;
        return -1;
    }

    return dirPosition;
}

int main(int argc, char* argv[]) {
    int dirPosition = set_args(argc, argv);
    if (dirPosition == -1) {
        return 0;
    }
    string path = argv[dirPosition];
    if (path.back() != '/') { path += '/'; }

    int fd = open(argv[dirPosition], O_RDONLY | O_DIRECTORY);
    if (fd == -1) {
        cout << "Error reading contents of " << path << endl;
        print_error();
        return 0;
    }
    visit(fd, path);
    close(fd);

    if (exec_target.empty()) {
        for (const auto &result : results) {
            cout << result << endl;
        }
    } else {
        std::vector<char*> c_results;
        c_results.reserve(results.size() + 2);
        c_results.push_back(const_cast<char*>(exec_target.c_str()));
        for (auto const& res : results) {
            c_results.push_back(const_cast<char*>(res.c_str()));
        }
        c_results.push_back(nullptr);

        int res = execv(c_results[0], c_results.data());
        if (res == -1) {
            cout << "Error executing " << exec_target << endl;
            print_error();
            return 0;
        }
    }
}