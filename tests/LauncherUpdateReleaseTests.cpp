#include "updateRelease.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cstdio>
#include <cstdlib>

namespace {

void Check(bool condition, const char* message) {
	if (!condition) {
		std::fprintf(stderr, "LauncherUpdateReleaseTests: %s\n", message);
		std::abort();
	}
}

QByteArray ReleaseResponse(const QString& tag, const QString& asset_name) {
	return QJsonDocument(QJsonObject {
	                         {QStringLiteral("tag_name"), tag},
	                         {QStringLiteral("published_at"), QStringLiteral("2026-11-25T13:06:41Z")},
	                         {QStringLiteral("draft"), false},
	                         {QStringLiteral("prerelease"), false},
	                         {QStringLiteral("body"), QStringLiteral("Bug fixes and improvements.")},
	                         {QStringLiteral("assets"),
	                          QJsonArray {QJsonObject {
	                              {QStringLiteral("name"), asset_name},
	                              {QStringLiteral("digest"),
	                               QStringLiteral("sha256:4c9907a4af5dc4b916151ee4beb471b741424f4d45fe458dd91bf2069c70d13d")},
	                              {QStringLiteral("size"), 42894301}}}}})
	    .toJson(QJsonDocument::Compact);
}

} // namespace

int main(int argc, char** argv) {
	QCoreApplication application(argc, argv);
	const auto tag = QStringLiteral("PulsePS5-2026-11-25-9957a04");
	const auto suffix = QStringLiteral("-Linux-x86_64.tar.gz");
	const auto asset = tag + suffix;

	Check(UpdateRelease::LATEST_URL ==
	          QStringLiteral("https://api.github.com/repos/Maksim1000-dev/PulsePS5/releases/latest"),
	      "latest release URL is not the PulsePS5 repository");
	Check(UpdateRelease::TAG_URL.arg(QStringLiteral("tag")) ==
	          QStringLiteral("https://api.github.com/repos/Maksim1000-dev/PulsePS5/releases/tags/tag"),
	      "tag URL is not the PulsePS5 repository");

	const auto release = UpdateRelease::Parse(ReleaseResponse(tag, asset), suffix);
	Check(release.error.isEmpty() && release.tag == tag, "valid release metadata was rejected");
	Check(release.notes == QStringLiteral("Bug fixes and improvements."),
	      "release notes were not parsed");
	Check(release.page_url ==
	          QUrl(QStringLiteral("https://github.com/Maksim1000-dev/PulsePS5/releases/tag/%1")
	                   .arg(tag)),
	      "release page URL is incorrect");
	Check(release.download_url ==
	          QUrl(QStringLiteral("https://github.com/Maksim1000-dev/PulsePS5/releases/download/%1/%2")
	                   .arg(tag, asset)),
	      "release asset URL is incorrect");
	Check(release.size == 42894301 && release.sha256.size() == 32,
	      "release size or SHA-256 digest was not parsed");

	const auto missing = UpdateRelease::Parse(ReleaseResponse(tag, QStringLiteral("other.zip")),
	                                          suffix);
	Check(!missing.error.isEmpty() && missing.download_url.isEmpty(),
	      "missing platform asset was accepted");
	Check(UpdateRelease::Compare(release, release) == UpdateRelease::VersionRelation::Current,
	      "matching release tags are not considered current");
	std::puts("LauncherUpdateReleaseTests: passed");
	return 0;
}
