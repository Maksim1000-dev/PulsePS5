#include "configuration.h"

#include <QCoreApplication>
#include <QJsonObject>
#include <QSettings>
#include <QTemporaryDir>

#include <cstdio>
#include <cstdlib>

namespace {

void Check(bool condition, const char* message) {
	if (!condition) {
		std::fprintf(stderr, "LauncherConfigurationTests: %s\n", message);
		std::abort();
	}
}

void CheckProfile(const Configuration& info) {
	Check(info.controller_override_enabled, "per-game controller profile was not enabled");
	Check(info.shader_failure_log_enabled, "shader failure log setting did not persist");
	Check(info.controller.color == QStringLiteral("#123456"), "controller color did not persist");
	Check(info.controller.speaker_volume == 37, "controller speaker volume did not persist");
	Check(info.controller.vibration_intensity == 64,
	      "controller vibration intensity did not persist");
	Check(info.controller.haptics_mode == Config::ControllerHapticsMode::Off,
	      "controller haptics mode did not persist");
	Check(!info.controller.adaptive_triggers_enabled,
	      "adaptive trigger setting did not persist");
	Check(!info.controller.speaker_enabled, "controller speaker setting did not persist");
	Check(info.controller.keyboard_player == 2, "keyboard player did not persist");
}

} // namespace

int main(int argc, char** argv) {
	QCoreApplication application(argc, argv);
	Configuration    original;
	original.controller_override_enabled             = true;
	original.controller.color                        = QStringLiteral("#123456");
	original.controller.speaker_volume               = 37;
	original.controller.vibration_intensity          = 64;
	original.controller.haptics_mode                 = Config::ControllerHapticsMode::Off;
	original.controller.adaptive_triggers_enabled    = false;
	original.controller.speaker_enabled              = false;
	original.controller.keyboard_player              = 2;
	original.shader_failure_log_enabled              = true;

	Configuration imported;
	QString       error;
	const auto    exported = QJsonObject::fromVariantMap(original.GameSettings());
	Check(imported.SetGameSettings(exported, error), "controller profile JSON import failed");
	CheckProfile(imported);

	QTemporaryDir directory;
	Check(directory.isValid(), "temporary settings directory could not be created");
	QSettings settings(directory.filePath(QStringLiteral("profile.ini")), QSettings::IniFormat);
	original.WriteSettings(&settings);
	settings.sync();
	Check(settings.status() == QSettings::NoError, "controller profile settings could not be saved");

	Configuration loaded;
	loaded.ReadSettings(&settings);
	CheckProfile(loaded);

	Configuration legacy;
	Check(!legacy.controller_override_enabled,
	      "legacy game settings unexpectedly enable a controller profile");
	std::puts("LauncherConfigurationTests: passed");
	return 0;
}
