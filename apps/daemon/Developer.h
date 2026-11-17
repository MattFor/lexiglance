#ifndef LEXIGLANCE_DAEMON_DEVELOPER_H
#define LEXIGLANCE_DAEMON_DEVELOPER_H

#include "IpcServer.h"

#include <lexiglance/core/Health.h>
#include <lexiglance/core/Json.h>
#include <lexiglance/platform/Platform.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// What developer mode asks of the daemon (docs/debugging.md): the pipeline trace, problems made up for trying the
// settings application, and the capture inspector's picture.
namespace lexiglance::daemon
{

	// The stages of a lookup, in their order.
	enum class Stage : std::uint8_t
	{
		Capture,
		Lookup,
		Translate,
		Render,
		Show,
	};

	// Sends the `trace` events, each stage with its median and 95th percentile over the latest lookups. Only while a
	// client subscribed to them (on()): nothing is timed or written otherwise. Thread-safe.
	class Tracer
	{
	public:
		using Fields = std::function<void( json::Writer& )>;

		struct Timing
		{
			Stage                    stage;
			std::chrono::nanoseconds took;
		};

		explicit Tracer( IpcServer* ipc = nullptr ) :
			ipc_( ipc )
		{
		}

		[[nodiscard]] bool on() const noexcept
		{
			return ipc_ != nullptr && ipc_->listening( IpcServer::Topic::Trace );
		}

		// The event of lookup `id` (its generation): its time, the stages timed, and what `fields` adds.
		[[nodiscard]] std::string event( std::uint64_t id, std::span<const Timing> timings, const Fields& fields = {} );

		void send( std::uint64_t id, std::initializer_list<Timing> timings, const Fields& fields = {} );

	private:
		static constexpr std::size_t stages = 5;
		static constexpr std::size_t kept   = 200;

		IpcServer*                              ipc_;
		std::mutex                              mutex_;
		std::array<std::vector<double>, stages> samples_;
		std::array<std::size_t, stages>         next_{};
	};

	[[nodiscard]] std::string_view stageName( Stage stage ) noexcept;

	[[nodiscard]] std::string base64( std::string_view data );

	// The names simulatedProblem() knows, for the settings application to offer.
	inline constexpr std::array<std::string_view, 5> simulated_problems{ "info", "warning", "error", "ocr", "permissions" };

	// A problem made up for trying the settings application ("info", "warning", "error": one of that severity with a fix;
	// "ocr": no OCR; "permissions": macOS's permissions refused); empty for an unknown name.
	[[nodiscard]] std::vector<health::Check> simulatedProblem( std::string_view name );

	// Puts simulated checks in place of the real ones of the same id, and the others at the end.
	void addSimulated( std::vector<health::Check>& checks, std::span<const health::Check> simulated );

	// The capture inspector's picture, PNG: what OCR read the text from with the lines it found (grey: found, not read;
	// green: read), the lines of the captured text (orange), the character under the pointer (blue) and the pointer (red).
	// Text boxes are in screen coordinates.
	[[nodiscard]] std::string inspectionPng( const platform::Snapshot& snapshot, platform::Point pointer, std::span<const platform::Rect> text, const platform::Rect& character );

} // namespace lexiglance::daemon

#endif // LEXIGLANCE_DAEMON_DEVELOPER_H
