#include "mod/CommandHistory.h"

#include "mod/CreatorTools.h"

#include "ll/api/event/EventBus.h"
#include "ll/api/event/render/UIRenderEvent.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/service/Bedrock.h"
#include "ll/api/thread/ClientThreadExecutor.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/network/ClientNetworkHandler.h"
#include "mc/network/packet/CommandOutputPacket.h"
#include "mc/server/commands/CommandRegistry.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace creator_tools {

namespace {

constexpr auto        kPollInterval    = std::chrono::milliseconds{500};
constexpr bool        kOnlyCommands    = true; // false would also persist ordinary chat messages
constexpr std::size_t kMaxSavedEntries = 100;

// First line of the history file: records whether the game keeps the newest entry at the front or the back
// of its deque. The rest of the file is always oldest -> newest, one command per line.
constexpr std::string_view kOrderKey = "# newest=";

std::filesystem::path historyFile() {
    return CreatorTools::getInstance().getSelf().getDataDir() / "command_history.txt";
}

bool shouldPersist(std::string const& entry) {
    if (entry.empty() || entry.find('\n') != std::string::npos || entry.find('\r') != std::string::npos) {
        return false;
    }
    return !kOnlyCommands || entry.front() == '/';
}

/// "/Gamemode creative" -> "gamemode"; also accepts a bare "gamemode" or "/gamemode".
std::string commandWord(std::string_view text) {
    if (!text.empty() && text.front() == '/') {
        text.remove_prefix(1);
    }
    std::string word{text.substr(0, text.find_first_of(" \t"))};
    for (auto& c : word) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return word;
}

std::string_view trimmed(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.remove_suffix(1);
    }
    return text;
}

/// True if the client's command registry knows the command word of `entry` ("/gamemode creative" -> "gamemode").
/// If the registry isn't available we can't tell, so the entry is accepted.
bool isKnownCommand(std::string const& entry) {
    auto registry = ll::service::getCommandRegistry(true);
    if (!registry) {
        return true;
    }

    auto const name = commandWord(entry);
    if (name.empty()) {
        return false;
    }
    if (registry->findCommand(name)) {
        return true;
    }
    if (auto const colon = name.find(':'); colon != std::string::npos) {
        return registry->findCommand(name.substr(colon + 1)) != nullptr;
    }
    return false;
}

/// A command the server answered with a syntax / unknown-command error. Filled by the packet hook (which may
/// run on another thread) and drained on the client thread by the poll.
struct Rejection {
    enum class Kind { Line, CommandName } kind;
    std::string text; // Line: the whole command line. CommandName: the unknown command word.
};

std::mutex             gRejectedMutex;
std::vector<Rejection> gRejected;

void queueRejection(Rejection rejection) {
    std::lock_guard lock{gRejectedMutex};
    if (gRejected.size() < 64) {
        gRejected.push_back(std::move(rejection));
    }
}

std::vector<Rejection> takeRejections() {
    std::lock_guard lock{gRejectedMutex};
    return std::exchange(gRejected, {});
}

/// Entries already in `trusted` are kept as-is so an unavailable or incomplete registry cannot wipe saved history.
std::deque<std::string> persistable(std::deque<std::string> const& history, std::deque<std::string> const& trusted) {
    std::deque<std::string> out;
    for (auto const& entry : history) {
        if (!shouldPersist(entry)) {
            continue;
        }
        if (std::ranges::find(trusted, entry) == trusted.end() && !isKnownCommand(entry)) {
            continue;
        }
        out.push_back(entry);
    }
    return out;
}

/// Works out the game's ordering by comparing two snapshots around one newly sent message. Returns
/// nullopt if the change isn't a clean single insertion (e.g. a duplicate that was moved), in which case
/// we simply try again on a later change.
///   newest at front: cur = [new] + prev            (minus prev's last entry once the list is full)
///   newest at back:  cur = prev + [new]            (minus prev's first entry once the list is full)
std::optional<bool> detectConvention(std::deque<std::string> const& prev, std::deque<std::string> const& cur) {
    if (prev.empty() || cur.size() < 2) {
        return std::nullopt;
    }
    if (cur.size() != prev.size() && cur.size() != prev.size() + 1) {
        return std::nullopt;
    }
    auto const m        = std::min(cur.size() - 1, prev.size());
    auto const frontNew = std::equal(cur.begin() + 1, cur.begin() + 1 + static_cast<std::ptrdiff_t>(m), prev.begin());
    auto const backNew  = std::equal(
        cur.begin(),
        cur.begin() + static_cast<std::ptrdiff_t>(m),
        prev.end() - static_cast<std::ptrdiff_t>(m)
    );
    if (frontNew == backNew) {
        return std::nullopt;
    }
    return frontNew;
}

struct SavedFile {
    std::vector<std::string> commands; // oldest first
    std::optional<bool>      newestAtFront;
};

SavedFile readSaved() {
    SavedFile     out;
    std::ifstream in(historyFile(), std::ios::binary);
    std::string   line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.starts_with(kOrderKey)) {
            auto const value = std::string_view{line}.substr(kOrderKey.size());
            if (value == "front") {
                out.newestAtFront = true;
            } else if (value == "back") {
                out.newestAtFront = false;
            }
            continue;
        }
        if (shouldPersist(line)) {
            out.commands.push_back(std::move(line));
        }
    }
    if (out.commands.size() > kMaxSavedEntries) {
        out.commands.erase(out.commands.begin(), out.commands.end() - static_cast<std::ptrdiff_t>(kMaxSavedEntries));
    }
    return out;
}

/// Adds `entry` as the newest item, replacing any older copy of the same command.
void insertEntry(std::deque<std::string>& history, std::string const& entry, bool newestAtFront) {
    std::erase(history, entry);
    if (newestAtFront) {
        history.push_front(entry);
    } else {
        history.push_back(entry);
    }
}

/// Writes `history` (game order) to disk, oldest first. Writes a temp file then renames it, so a crash
/// mid-write can't leave a half-written history behind.
bool save(std::deque<std::string> const& history, std::optional<bool> newestAtFront) {
    auto& log  = CreatorTools::getInstance().getSelf().getLogger();
    auto  file = historyFile();

    std::vector<std::string> ordered(history.begin(), history.end());
    if (newestAtFront.value_or(false)) {
        std::reverse(ordered.begin(), ordered.end());
    }
    if (ordered.size() > kMaxSavedEntries) {
        ordered.erase(ordered.begin(), ordered.end() - static_cast<std::ptrdiff_t>(kMaxSavedEntries));
    }

    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);

    auto temp  = file;
    temp      += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            log.warn("Cannot write command history to '{}'", temp.string());
            return false;
        }
        if (newestAtFront) {
            out << kOrderKey << (*newestAtFront ? "front" : "back") << '\n';
        }
        for (auto const& entry : ordered) {
            out << entry << '\n';
        }
    }
    std::filesystem::rename(temp, file, ec);
    if (ec) {
        log.warn("Cannot save command history: {}", ec.message());
        return false;
    }
    return true;
}

} // namespace

// The server answers every command with a CommandOutputPacket. Syntax errors (bad arguments) and unknown
// commands - including ones from addons / custom commands - are reported here, so we learn about them even
// though the client can't parse those commands itself.
LL_AUTO_TYPE_INSTANCE_HOOK(
    CommandOutputHook,
    ll::memory::HookPriority::Normal,
    ClientNetworkHandler,
    &ClientNetworkHandler::$handle,
    void,
    NetworkIdentifier const&   source,
    CommandOutputPacket const& packet
) {
    auto const& output = packet.mOutput.get();
    if (output.mSuccessCount == 0) {
        for (auto const& message : output.mMessages) {
            if (message.mType != CommandOutputMessageType::Error) {
                continue;
            }
            if (message.mMessageId == "commands.generic.syntax") {
                // params are: text before the error, the offending token, text after it -> the original line
                std::string line;
                for (auto const& param : message.mParams) {
                    line += param;
                }
                queueRejection({Rejection::Kind::Line, std::move(line)});
            } else if (message.mMessageId == "commands.generic.unknown" && !message.mParams.empty()) {
                queueRejection({Rejection::Kind::CommandName, message.mParams.front()});
            }
        }
    }
    origin(source, packet);
}

bool CommandHistory::enable() {
    mLoaded      = false;
    mPendingSave = false;
    mNewestAtFront.reset();
    mLastRaw.clear();
    mLastSaved.clear();
    mNextPoll = {};

    mRenderListener = ll::event::EventBus::getInstance().emplaceListener<ll::event::BeforeUIRenderEvent>(
        [this](ll::event::BeforeUIRenderEvent&) {
            auto const now = std::chrono::steady_clock::now();
            if (now < mNextPoll) {
                return;
            }
            mNextPoll = now + kPollInterval;
            if (mPollQueued.exchange(true, std::memory_order_acq_rel)) {
                return;
            }

            auto alive = CreatorTools::getInstance().alive();
            ll::thread::ClientThreadExecutor::getDefault().execute([this, alive] {
                mPollQueued.store(false, std::memory_order_release);
                if (alive->load()) {
                    pollOnClientThread();
                }
            });
        }
    );
    return true;
}

void CommandHistory::disable() { mRenderListener.reset(); }

void CommandHistory::pollOnClientThread() {
    auto client = ll::service::getClientInstance();
    if (!client) {
        return; // try again on the next poll
    }

    auto& log     = CreatorTools::getInstance().getSelf().getLogger();
    auto& history = client->getSentMessageHistory();

    if (!mLoaded) {
        mLoaded = true;
        std::deque<std::string> trusted;
        if (history.empty()) {
            auto const saved = readSaved();
            mNewestAtFront   = saved.newestAtFront;
            // Without a recorded order (first run, or only one entry saved) the order can't matter yet, or
            // is assumed to be newest-at-back until the first real commands let us learn it.
            for (auto const& command : saved.commands) {
                insertEntry(history, command, mNewestAtFront.value_or(false));
            }
            trusted = history;
            log.info("Restored {} command(s) from {}", saved.commands.size(), historyFile().string());
        } else {
            log.warn("Chat history already has entries; skipping restore to avoid scrambling the order");
        }
        mLastRaw   = history;
        mLastSaved = persistable(history, trusted);
        return;
    }

    // Drop commands the server rejected, from the game's own history as well so Up arrow doesn't offer them.
    for (auto const& rejection : takeRejections()) {
        auto const matches = [&](std::string const& entry) {
            if (!shouldPersist(entry)) {
                return false;
            }
            return rejection.kind == Rejection::Kind::Line ? trimmed(entry) == trimmed(rejection.text)
                                                           : commandWord(entry) == commandWord(rejection.text);
        };
        auto const removed = std::erase_if(history, matches);
        std::erase_if(mLastRaw, matches); // keep the snapshot in step so the removal isn't seen as a "change"
        if (removed > 0) {
            log.info("Dropped {} rejected command(s) from history: {}", removed, rejection.text);
        } else {
            log.debug("Server rejected '{}' but no matching history entry was found", rejection.text);
        }
    }

    if (history == mLastRaw && !mPendingSave) {
        return;
    }

    if (history != mLastRaw) {
        if (!mNewestAtFront) {
            mNewestAtFront = detectConvention(mLastRaw, history);
            if (mNewestAtFront) {
                log.info("Chat history order learned: newest entry is at the {}", *mNewestAtFront ? "front" : "back");
            }
        }
        mLastRaw = history;
    }

    auto current = persistable(history, mLastSaved);
    if (current == mLastSaved && !mPendingSave) {
        return;
    }
    // With two or more commands the file order depends on the game's ordering; wait until it is known.
    if (!mNewestAtFront && current.size() > 1) {
        mPendingSave = true;
        return;
    }
    if (save(current, mNewestAtFront)) {
        mLastSaved   = std::move(current);
        mPendingSave = false;
    }
}

} // namespace creator_tools
