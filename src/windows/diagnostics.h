#pragma once
#include <filesystem>
#include <string>
#include <string_view>

namespace partymic::diagnostics {
// Bounded, best-effort queue. Producers never wait for disk or the writer lock.
// Lifecycle events only on audio workers; periodic metrics come from the monitor.
void start(std::string_view version, const std::filesystem::path& directory = {}) noexcept;
void write(std::string_view event, std::string_view detail = {}) noexcept;
void write(std::string_view event, const std::wstring& detail) noexcept;
void stop() noexcept;
bool flush(unsigned timeout_ms = 1000) noexcept;
std::filesystem::path folder();
std::string utf8(std::wstring_view text);
}
