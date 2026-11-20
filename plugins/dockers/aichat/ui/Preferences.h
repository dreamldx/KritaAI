/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef _UI_PREFERENCES_H_
#define _UI_PREFERENCES_H_

#include <kis_preference_set_registry.h>

class QCheckBox;
class QLabel;
class QLineEdit;

namespace AiChat {


/**
 * The "AI Chat" page of Configure Krita: where the AI service is found.
 *
 * Configure Krita calls loadPreferences() when it opens, savePreferences()
 * when the user accepts and loadDefaultPreferences() for "Restore Defaults".
 * The values are kept in an Config.
 */
class Preferences : public KisPreferenceSet
{
    Q_OBJECT
public:
    explicit Preferences(QWidget *parent = nullptr);

    QString id() override;
    QString name() override;
    QString header() override;
    QIcon icon() override;

public Q_SLOTS:
    void savePreferences() const override;
    void loadPreferences() override;
    void loadDefaultPreferences() override;

private Q_SLOTS:
    void slotUrlChanged();

private:
    QLineEdit *m_baseUrl {nullptr};
    QLabel *m_urlHint {nullptr};
    QLineEdit *m_apiKey {nullptr};
    QCheckBox *m_showKey {nullptr};
    QLineEdit *m_model {nullptr};
    QCheckBox *m_sendEffort {nullptr};

    // the model that looks at pictures for the AI ("ask_about_image")
    QLineEdit *m_visionUrl {nullptr};
    QLineEdit *m_visionApiKey {nullptr};
    QLineEdit *m_visionModel {nullptr};
    QCheckBox *m_visionNoThinking {nullptr};
    QLineEdit *m_visionBlacklist {nullptr};

    // fal.ai, for the Topaz tools
    QLineEdit *m_falKey {nullptr};
};

class PreferencesFactory : public KisAbstractPreferenceSetFactory
{
public:
    KisPreferenceSet *createPreferenceSet() override;
    QString id() const override;
};

} // namespace AiChat

#endif
