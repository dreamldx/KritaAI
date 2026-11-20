/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QApplication>
#include <QCheckBox>
#include <QLabel>
#include <QLineEdit>
#include <QStandardPaths>
#include <QTest>

#include <memory>

#include "../config/Config.h"
#include "../ui/Preferences.h"

using namespace AiChat;

class PreferencesTest : public QObject
{
    Q_OBJECT

private:
    static QLineEdit *edit(QWidget *page, const char *name)
    {
        return page->findChild<QLineEdit *>(QString::fromLatin1(name));
    }

    static bool hintContains(QWidget *page, const QString &text)
    {
        for (QLabel *label : page->findChildren<QLabel *>()) {
            if (label->text().contains(text)) {
                return true;
            }
        }
        return false;
    }

private Q_SLOTS:
    void testDefaults()
    {
        const Config defaults = Config::defaults();
        QCOMPARE(defaults.baseUrl, QString("https://api.openai.com/v1"));
        QVERIFY(defaults.apiKey.isEmpty());
        QVERIFY(!defaults.model.isEmpty());
        QVERIFY(!defaults.sendReasoningEffort);
        QVERIFY(defaults.isComplete());
    }

    void testConfigRoundTrip()
    {
        Config config;
        config.baseUrl = "http://localhost:11434/v1";
        config.apiKey = "sk-roundtrip";
        config.model = "llama3";
        config.sendReasoningEffort = true;
        config.save();

        const Config loaded = Config::load();
        QCOMPARE(loaded.baseUrl, config.baseUrl);
        QCOMPARE(loaded.apiKey, config.apiKey);
        QCOMPARE(loaded.model, config.model);
        QCOMPARE(loaded.sendReasoningEffort, true);
    }

    void testIncompleteConfig()
    {
        Config config = Config::defaults();
        config.model = "  ";
        QVERIFY(!config.isComplete());

        config = Config::defaults();
        config.baseUrl = "not a url";
        QVERIFY(!config.isComplete());

        config = Config::defaults();
        config.apiKey.clear(); // local servers need no key
        QVERIFY(config.isComplete());
    }

    void testPageShowsTheSavedSettings()
    {
        Config config;
        config.baseUrl = "https://example.org/v1";
        config.apiKey = "sk-shown";
        config.model = "some-model";
        config.sendReasoningEffort = true;
        config.save();

        std::unique_ptr<Preferences> page(new Preferences());
        QCOMPARE(edit(page.get(), "aiChatBaseUrl")->text(), QString("https://example.org/v1"));
        QCOMPARE(edit(page.get(), "aiChatApiKey")->text(), QString("sk-shown"));
        QCOMPARE(edit(page.get(), "aiChatModel")->text(), QString("some-model"));
        QVERIFY(page->findChild<QCheckBox *>("aiChatSendEffort")->isChecked());
    }

    void testPageSavesWhatTheUserTyped()
    {
        std::unique_ptr<Preferences> page(new Preferences());
        // with spaces around, as pasted text often has
        edit(page.get(), "aiChatBaseUrl")->setText("  https://my.server/v1/  ");
        edit(page.get(), "aiChatApiKey")->setText("  sk-typed\n");
        edit(page.get(), "aiChatModel")->setText(" my-model ");
        page->findChild<QCheckBox *>("aiChatSendEffort")->setChecked(true);
        page->savePreferences();

        const Config loaded = Config::load();
        QCOMPARE(loaded.baseUrl, QString("https://my.server/v1/"));
        QCOMPARE(loaded.apiKey, QString("sk-typed"));
        QCOMPARE(loaded.model, QString("my-model"));
        QVERIFY(loaded.sendReasoningEffort);
        QCOMPARE(loaded.chatCompletionsUrl().toString(), QString("https://my.server/v1/chat/completions"));
    }

    void testVisionModelIsSavedAndLoaded()
    {
        QVERIFY(!Config::defaults().hasVision()); // off until the user sets it up

        std::unique_ptr<Preferences> page(new Preferences());
        edit(page.get(), "aiChatVisionUrl")->setText(" https://api.deepseek.com ");
        edit(page.get(), "aiChatVisionApiKey")->setText("sk-vision");
        edit(page.get(), "aiChatVisionModel")->setText(" some-vision-model ");
        page->savePreferences();

        const Config loaded = Config::load();
        QVERIFY(loaded.hasVision());
        QCOMPARE(loaded.visionBaseUrl, QString("https://api.deepseek.com"));
        QCOMPARE(loaded.visionApiKey, QString("sk-vision"));
        QCOMPARE(loaded.visionModel, QString("some-vision-model"));
        QVERIFY(loaded.visionDisableThinking); // on by default

        // the choice can be turned off, and is remembered
        QVERIFY(page->findChild<QCheckBox *>("aiChatVisionNoThinking")->isChecked());
        page->findChild<QCheckBox *>("aiChatVisionNoThinking")->setChecked(false);
        page->savePreferences();
        QVERIFY(!Config::load().visionDisableThinking);
        page->findChild<QCheckBox *>("aiChatVisionNoThinking")->setChecked(true);
        page->savePreferences();
        QVERIFY(Config::load().visionDisableThinking);

        std::unique_ptr<Preferences> reopened(new Preferences());
        QCOMPARE(edit(reopened.get(), "aiChatVisionModel")->text(), QString("some-vision-model"));

        // an address without a model (or the other way round) is not enough
        Config half = loaded;
        half.visionModel.clear();
        QVERIFY(!half.hasVision());
        half = loaded;
        half.visionBaseUrl = "not a url";
        QVERIFY(!half.hasVision());

        // "Restore defaults" switches it off again, once saved
        reopened->loadDefaultPreferences();
        QVERIFY(edit(reopened.get(), "aiChatVisionUrl")->text().isEmpty());
        reopened->savePreferences();
        QVERIFY(!Config::load().hasVision());
    }

    void testProviderListIsParsedTidily()
    {
        QCOMPARE(Config::parseProviderList(""), QStringList());
        QCOMPARE(Config::parseProviderList("  ,; \n"), QStringList());
        QCOMPARE(Config::parseProviderList("DeepInfra"), QStringList {"DeepInfra"});
        // commas, semicolons and lines all separate; spaces around are dropped; names keep their spelling
        QCOMPARE(Config::parseProviderList(" DeepInfra ,Together;\nFireworks\r\n"),
                 (QStringList {"DeepInfra", "Together", "Fireworks"}));
        // the same company twice (whatever the case) counts once
        QCOMPARE(Config::parseProviderList("Together, together, TOGETHER"), QStringList {"Together"});
        // a name with a space inside is one name
        QCOMPARE(Config::parseProviderList("Sail Research, Open Inference"),
                 (QStringList {"Sail Research", "Open Inference"}));
    }

    void testVisionBlacklistIsSavedLoadedAndTidied()
    {
        QVERIFY(Config::defaults().visionIgnoredProviders().isEmpty());

        std::unique_ptr<Preferences> page(new Preferences());
        edit(page.get(), "aiChatVisionBlacklist")->setText(" DeepInfra;Together ,  ,together\nFireworks ");
        page->savePreferences();

        const Config loaded = Config::load();
        QCOMPARE(loaded.visionProviderBlacklist, QString("DeepInfra, Together, Fireworks")); // saved as one tidy line
        QCOMPARE(loaded.visionIgnoredProviders(), (QStringList {"DeepInfra", "Together", "Fireworks"}));

        std::unique_ptr<Preferences> reopened(new Preferences());
        QCOMPARE(edit(reopened.get(), "aiChatVisionBlacklist")->text(), QString("DeepInfra, Together, Fireworks"));

        // "Restore defaults" empties it, once saved
        reopened->loadDefaultPreferences();
        QVERIFY(edit(reopened.get(), "aiChatVisionBlacklist")->text().isEmpty());
        reopened->savePreferences();
        QVERIFY(Config::load().visionIgnoredProviders().isEmpty());
    }

    void testFalKeyIsSavedLoadedAndHidden()
    {
        QVERIFY(Config::defaults().falApiKey.isEmpty()); // the Topaz tools are off until a key is set

        std::unique_ptr<Preferences> page(new Preferences());
        QLineEdit *key = edit(page.get(), "aiChatFalKey");
        QVERIFY(key);
        QCOMPARE(key->echoMode(), QLineEdit::Password);
        page->findChild<QCheckBox *>("aiChatShowKey")->setChecked(true); // the "Show" box covers it too
        QCOMPARE(key->echoMode(), QLineEdit::Normal);

        key->setText("  fal-key:secret \n");
        page->savePreferences();
        QCOMPARE(Config::load().falApiKey, QString("fal-key:secret"));

        std::unique_ptr<Preferences> reopened(new Preferences());
        QCOMPARE(edit(reopened.get(), "aiChatFalKey")->text(), QString("fal-key:secret"));

        reopened->loadDefaultPreferences();
        QVERIFY(edit(reopened.get(), "aiChatFalKey")->text().isEmpty());
        reopened->savePreferences();
        QVERIFY(Config::load().falApiKey.isEmpty());
    }

    void testRestoreDefaultsOnlyChangesThePageUntilItIsSaved()
    {
        Config config;
        config.baseUrl = "https://changed.example/v1";
        config.apiKey = "sk-keep";
        config.model = "changed";
        config.save();

        std::unique_ptr<Preferences> page(new Preferences());
        page->loadDefaultPreferences();
        QCOMPARE(edit(page.get(), "aiChatBaseUrl")->text(), Config::defaults().baseUrl);
        QCOMPARE(edit(page.get(), "aiChatModel")->text(), Config::defaults().model);
        QVERIFY(edit(page.get(), "aiChatApiKey")->text().isEmpty());

        // nothing was saved yet
        QCOMPARE(Config::load().model, QString("changed"));

        page->savePreferences();
        QCOMPARE(Config::load().model, Config::defaults().model);
    }

    void testCancelingLeavesTheSettingsAlone()
    {
        Config config;
        config.baseUrl = "https://kept.example/v1";
        config.model = "kept";
        config.save();

        std::unique_ptr<Preferences> page(new Preferences());
        edit(page.get(), "aiChatModel")->setText("typed but never accepted");
        page.reset(); // the dialog was canceled: savePreferences() was not called

        QCOMPARE(Config::load().model, QString("kept"));
    }

    void testBadAddressIsExplained()
    {
        std::unique_ptr<Preferences> page(new Preferences());

        edit(page.get(), "aiChatBaseUrl")->setText("api.openai.com/v1");
        QVERIFY(hintContains(page.get(), "not a usable address"));

        edit(page.get(), "aiChatBaseUrl")->setText("https://api.openai.com/v1");
        QVERIFY(!hintContains(page.get(), "not a usable address"));
        QVERIFY(hintContains(page.get(), "https://api.openai.com/v1/chat/completions"));
    }

    void testKeyIsHiddenUntilShown()
    {
        std::unique_ptr<Preferences> page(new Preferences());
        QLineEdit *key = edit(page.get(), "aiChatApiKey");
        QCOMPARE(key->echoMode(), QLineEdit::Password);

        page->findChild<QCheckBox *>("aiChatShowKey")->setChecked(true);
        QCOMPARE(key->echoMode(), QLineEdit::Normal);
        page->findChild<QCheckBox *>("aiChatShowKey")->setChecked(false);
        QCOMPARE(key->echoMode(), QLineEdit::Password);
    }

    void testPageDescribesItself()
    {
        PreferencesFactory factory;
        std::unique_ptr<KisPreferenceSet> page(factory.createPreferenceSet());
        QVERIFY(page);
        QCOMPARE(page->name(), QString("AI Chat"));
        QCOMPARE(page->header(), QString("AI Chat"));
        QCOMPARE(page->id(), factory.id());
        // Only the call is checked: the icons of Krita are compiled into the application, which a
        // test program does not have, so the icon is empty here. Look at it in Krita itself.
        page->icon();
    }
};

int main(int argc, char **argv)
{
    // never touch the real settings of the person running the tests
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);
    PreferencesTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "PreferencesTest.moc"
