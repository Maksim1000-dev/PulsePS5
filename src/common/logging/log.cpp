#include "common/logging/log.h"

#include "common/emulatorConfig.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <utility>
#include <spdlog/logger.h>
#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_sinks.h>

namespace Log {

static bool g_initialized = false;
static bool g_console     = false;
// Null when silent. Only Initialize sets it, before other threads start. Never destroyed, since
// threads keep logging while exit runs static destructors.
static spdlog::logger* g_logger = nullptr;

struct ShaderDiagnosticContext {
	bool        active = false;
	uint64_t    hash   = 0;
	std::string stage;
	std::string phase;
};

static thread_local ShaderDiagnosticContext g_shader_diagnostic_context;
static std::mutex                            g_shader_diagnostic_mutex;

static void WriteStdout(std::string_view text, fmt::text_style style) {
	if (style == fmt::text_style {}) {
		std::fwrite(text.data(), 1, text.size(), stdout);
	} else {
		fmt::print(stdout, style, "{}", text);
	}
	std::fflush(stdout);
}

void Initialize() {
	g_initialized        = true;
	const auto direction = Config::GetPrintfDirection();
	const auto path      = Config::GetPrintfOutputFile();
	g_console            = direction == Config::LogDirection::Console;
	delete g_logger;
	g_logger = nullptr;

	spdlog::sink_ptr sink;
	if (g_console) {
		sink = std::make_shared<spdlog::sinks::stdout_sink_mt>();
	} else if (direction == Config::LogDirection::File && !path.empty()) {
		if (path.has_parent_path()) {
			std::filesystem::create_directories(path.parent_path());
		}
		const bool truncate = std::getenv(RelaunchedVariable) == nullptr;
		sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path.native(), truncate);
	}
	if (sink != nullptr) {
		sink->set_formatter(std::make_unique<spdlog::pattern_formatter>(
		    "%v", spdlog::pattern_time_type::local, ""));
		g_logger = new spdlog::logger("kyty", std::move(sink));
	}
	if (Config::ShaderFailureLogEnabled()) {
		std::error_code error;
		std::filesystem::remove(Config::GetShaderLogFolder() / "shader-failure.log", error);
		if (error) {
			WriteToConsoleAndLog(fmt::format("Could not remove previous shader failure report: {}\n",
			                                  error.message()));
		}
	}
}

void Flush() {
	if (g_logger != nullptr) {
		g_logger->flush();
	}
}

bool IsSilent() {
	// Before Initialize the text goes to stdout.
	return g_initialized && g_logger == nullptr;
}

void Write(fmt::text_style style, std::string_view text) {
	if (!g_initialized) {
		WriteStdout(text, style);
		return;
	}
	if (g_logger == nullptr) {
		return;
	}
	if (g_console && style != fmt::text_style {}) {
		fmt::memory_buffer styled;
		fmt::format_to(std::back_inserter(styled), style, "{}", text);
		g_logger->log(spdlog::level::info, spdlog::string_view_t(styled.data(), styled.size()));
	} else {
		g_logger->log(spdlog::level::info, spdlog::string_view_t(text.data(), text.size()));
	}
}

void WriteToConsoleAndLog(std::string_view text) {
	if (g_initialized && !g_console) {
		WriteStdout(text, {});
	}
	Write({}, text);
	Flush();
}

ShaderDiagnosticScope::ShaderDiagnosticScope(std::string_view stage, uint64_t shader_hash) {
	if (!Config::ShaderFailureLogEnabled()) {
		return;
	}
	m_enabled         = true;
	m_previous_active = g_shader_diagnostic_context.active;
	m_previous_hash   = g_shader_diagnostic_context.hash;
	m_previous_stage  = std::move(g_shader_diagnostic_context.stage);
	m_previous_phase  = std::move(g_shader_diagnostic_context.phase);
	g_shader_diagnostic_context.active = true;
	g_shader_diagnostic_context.hash   = shader_hash;
	g_shader_diagnostic_context.stage  = std::string(stage);
	g_shader_diagnostic_context.phase.clear();
}

ShaderDiagnosticScope::~ShaderDiagnosticScope() {
	if (!m_enabled) {
		return;
	}
	g_shader_diagnostic_context.active = m_previous_active;
	g_shader_diagnostic_context.hash   = m_previous_hash;
	g_shader_diagnostic_context.stage  = std::move(m_previous_stage);
	g_shader_diagnostic_context.phase  = std::move(m_previous_phase);
}

void ShaderDiagnosticScope::SetPhase(std::string_view phase) {
	if (m_enabled) {
		g_shader_diagnostic_context.phase = std::string(phase);
	}
}

void SetShaderDiagnosticPhase(std::string_view phase) {
	if (g_shader_diagnostic_context.active) {
		g_shader_diagnostic_context.phase = std::string(phase);
	}
}

void WriteShaderFailureDiagnostic(const char* file, int line, std::string_view reason) {
	if (!Config::ShaderFailureLogEnabled() || !g_shader_diagnostic_context.active) {
		return;
	}

	const auto folder = Config::GetShaderLogFolder();
	if (folder.empty()) {
		WriteToConsoleAndLog("Could not write shader failure report: shader log folder is empty.\n");
		return;
	}
	const auto path = folder / "shader-failure.log";
	std::lock_guard lock(g_shader_diagnostic_mutex);
	std::error_code error;
	std::filesystem::create_directories(folder, error);
	if (error) {
		WriteToConsoleAndLog(fmt::format("Could not create shader failure report folder: {}\n",
		                                  error.message()));
		return;
	}

	std::ofstream report(path, std::ios::out | std::ios::trunc);
	if (!report) {
		WriteToConsoleAndLog("Could not open shader failure report for writing.\n");
		return;
	}
	report << "PulsePS5 shader failure\n"
	       << "Stage: " << g_shader_diagnostic_context.stage << '\n'
	       << fmt::format("Shader hash: 0x{:016x}\n", g_shader_diagnostic_context.hash)
	       << "Last step: " << g_shader_diagnostic_context.phase << '\n'
	       << "Location: " << file << ':' << line << "\n\n"
	       << reason << '\n';
	report.flush();
	if (!report) {
		WriteToConsoleAndLog("Writing the shader failure report failed.\n");
	}
}

} // namespace Log
