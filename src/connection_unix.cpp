#include "connection.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <dirent.h>

#include <queue>
#include <string>
#include <utility>
#include <functional>

int GetProcessId()
{
    return ::getpid();
}

struct BaseConnectionUnix : public BaseConnection {
    int sock{-1};
};

static BaseConnectionUnix Connection;
static sockaddr_un PipeAddr{};
#ifdef MSG_NOSIGNAL
static int MsgFlags = MSG_NOSIGNAL;
#else
static int MsgFlags = 0;
#endif

static std::string IpcFilenamePrefix = "discord-ipc-";
static std::string IpcExtraRootDirPrefixes[] = {"snap.", ".flatpak"};

static const char* GetTempPath()
{
    const char* temp = getenv("XDG_RUNTIME_DIR");
    temp = temp ? temp : getenv("TMPDIR");
    temp = temp ? temp : getenv("TMP");
    temp = temp ? temp : getenv("TEMP");
    temp = temp ? temp : "/tmp";
    return temp;
}

class directory_iterator {
public:
    directory_iterator(std::string const& path)
      : m_path{path}
    {
    }
    directory_iterator(std::string&& path)
      : m_path{std::move(path)}
    {
    }
    directory_iterator(const directory_iterator& o)
      : m_path{o.m_path}
    {
    }
    directory_iterator(directory_iterator&& o) noexcept
      : m_path(std::move(o.m_path))
      , m_stream(std::exchange(o.m_stream, nullptr))
    {
    }
    directory_iterator& operator=(const directory_iterator& o)
    {
        if (this == &o)
            return *this;
        close();
        this->m_path = o.m_path;
        return *this;
    }
    directory_iterator& operator=(directory_iterator&& o) noexcept
    {
        if (this == &o)
            return *this;
        close();
        m_path = std::move(o.m_path);
        m_stream = std::exchange(o.m_stream, nullptr);
        return *this;
    }
    ~directory_iterator() { close(); }
    std::string const& path() const { return m_path; }
    DIR* stream() const { return m_stream; }
    explicit operator bool() const noexcept { return m_stream != nullptr; }
    bool open()
    {
        close();
        m_stream = opendir(m_path.c_str());
        return m_stream != nullptr;
    }
    struct dirent* next()
    {
        if (!m_stream) {
            return nullptr;
        }
        auto result = readdir(m_stream);
        if (!result) {
            close();
        }
        return result;
    }
    void close()
    {
        if (m_stream) {
            closedir(m_stream);
            m_stream = nullptr;
        }
    }

private:
    std::string m_path;
    DIR* m_stream{nullptr};
};

static bool discord_ipc_directory_predicate(std::string const& root_parent,
                                            std::string const& parent,
                                            const char* directory)
{
    if (parent != root_parent)
        return true;
    for (std::string const& prefix : IpcExtraRootDirPrefixes)
        if (prefix.compare(0, std::string::npos, directory, prefix.size()) == 0)
            return true;
    return false;
}

static bool discord_ipc_file_predicate(struct dirent* entry)
{
    return entry->d_type == DT_SOCK &&
      strncmp(IpcFilenamePrefix.c_str(), entry->d_name, IpcFilenamePrefix.size()) == 0 &&
      isdigit(entry->d_name[IpcFilenamePrefix.size()]);
}

static std::string resolve_path(std::string const& dir, struct dirent* entry)
{
    std::string file;
    file.reserve(dir.size() + 1 + sizeof(entry->d_name) + 1);
    file.append(dir);
    file.append("/");
    file.append(entry->d_name);
    return file;
}

// Reads the next iterator entry and sees if it satisfies the predicate.
// If there is no next entry, this function returns an empty string. If all
// remaining files read by this iterator are not a matching file, the next
// directory is popped from the queue, the iterator is set to that directory,
// it is opened and the function proceeds with the search using this iterator.
// This is done until either a matching file is found or the queue is empty.
// Once an entry satisfies the predicate, the full file path is returned.
// Otherwise an empty string is returned.
static std::string directory_find_next_recursive(
  directory_iterator& directory,
  std::function<bool(std::string const& parent, const char* directory)> directory_predicate,
  std::function<bool(struct dirent* entry)> filename_predicate,
  std::queue<std::string>& directory_queue)
{
    while (directory) {
        while (auto entry = directory.next()) {
            if (entry->d_type == DT_DIR) {
                if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                    continue;
                }
                if (!directory_predicate(directory.path(), entry->d_name)) {
                    continue;
                }
                directory_queue.push(resolve_path(directory.path(), entry));
                continue;
            }
            if (filename_predicate(entry)) {
                auto res = resolve_path(directory.path(), entry);
                return res;
            }
        }
        directory.close();
        while (!directory && !directory_queue.empty()) {
            directory = directory_iterator(std::move(directory_queue.front()));
            directory.open();
            directory_queue.pop();
        }
    }
    return "";
}

/*static*/ BaseConnection* BaseConnection::Create()
{
    PipeAddr.sun_family = AF_UNIX;
    return &Connection;
}

/*static*/ void BaseConnection::Destroy(BaseConnection*& c)
{
    auto self = reinterpret_cast<BaseConnectionUnix*>(c);
    self->Close();
    c = nullptr;
}

bool BaseConnection::Open()
{
    const char* tempPath = GetTempPath();
    auto self = reinterpret_cast<BaseConnectionUnix*>(this);
    self->sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (self->sock == -1) {
        return false;
    }
    fcntl(self->sock, F_SETFL, O_NONBLOCK);
#ifdef SO_NOSIGPIPE
    int optval = 1;
    setsockopt(self->sock, SOL_SOCKET, SO_NOSIGPIPE, &optval, sizeof(optval));
#endif
    directory_iterator directory(tempPath);
    directory.open();
    std::queue<std::string> queue;
    auto directory_predicate = std::bind(
      discord_ipc_directory_predicate, tempPath, std::placeholders::_1, std::placeholders::_2);
    while (true) {
        std::string path = directory_find_next_recursive(
          directory, directory_predicate, discord_ipc_file_predicate, queue);
        if (path.empty()) {
            break;
        }
        snprintf(PipeAddr.sun_path, sizeof(PipeAddr.sun_path), "%s", path.c_str());
        int err = connect(self->sock, (const sockaddr*)&PipeAddr, sizeof(PipeAddr));
        if (err == 0) {
            self->isOpen = true;
            return true;
        }
    }
    self->Close();
    return false;
}

bool BaseConnection::Close()
{
    auto self = reinterpret_cast<BaseConnectionUnix*>(this);
    if (self->sock == -1) {
        return false;
    }
    close(self->sock);
    self->sock = -1;
    self->isOpen = false;
    return true;
}

bool BaseConnection::Write(const void* data, size_t length)
{
    auto self = reinterpret_cast<BaseConnectionUnix*>(this);

    if (self->sock == -1) {
        return false;
    }

    ssize_t sentBytes = send(self->sock, data, length, MsgFlags);
    if (sentBytes < 0) {
        Close();
    }
    return sentBytes == (ssize_t)length;
}

bool BaseConnection::Read(void* data, size_t length)
{
    auto self = reinterpret_cast<BaseConnectionUnix*>(this);

    if (self->sock == -1) {
        return false;
    }

    int res = (int)recv(self->sock, data, length, MsgFlags);
    if (res < 0) {
        if (errno == EAGAIN) {
            return false;
        }
        Close();
    }
    else if (res == 0) {
        Close();
    }
    return res == (int)length;
}
