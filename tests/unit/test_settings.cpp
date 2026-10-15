#include <gtest/gtest.h>

#include <QTemporaryDir>

#include "config/AppSettings.h"

using namespace blendnet;

TEST(AppSettings, KnownSourcesRoundTrip) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString ini = dir.filePath("cfg.ini");

    QList<KnownSource> in{{"render.example.org", 7878, "tok1", ""}, {"192.0.2.10", 9000, "tok2", "ab:cd"}};
    {
        AppSettings s(ini);
        s.setKnownSources(in);
        s.sync();
    }
    AppSettings s2(ini);
    EXPECT_EQ(s2.knownSources(), in);
}

TEST(AppSettings, Defaults) {
    QTemporaryDir dir;
    AppSettings s(dir.filePath("empty.ini"));
    EXPECT_EQ(s.listenPort(), AppSettings::kDefaultPort);
    EXPECT_EQ(s.logLevel(), LogLevel::Info);
    EXPECT_TRUE(s.knownSources().isEmpty());
}
