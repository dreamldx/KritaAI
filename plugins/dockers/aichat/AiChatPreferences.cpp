/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "AiChatPreferences.h"

#include <QCheckBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QVBoxLayout>

#include <kis_icon_utils.h>
#include <klocalizedstring.h>

#include "AiChatConfig.h"

AiChatPreferences::AiChatPreferences(QWidget *parent)
    : KisPreferenceSet(parent)
{
    // server address, with a hint that also tells when the address cannot be used
    m_baseUrl = new QLineEdit();
    m_baseUrl->setObjectName(QStringLiteral("aiChatBaseUrl"));
    m_baseUrl->setPlaceholderText(QStringLiteral("https://api.openai.com/v1"));
    m_baseUrl->setClearButtonEnabled(true);

    m_urlHint = new QLabel();
    m_urlHint->setWordWrap(true);
    m_urlHint->setEnabled(false); // dimmed

    // API key, hidden as it is typed
    m_apiKey = new QLineEdit();
    m_apiKey->setObjectName(QStringLiteral("aiChatApiKey"));
    m_apiKey->setEchoMode(QLineEdit::Password);
    m_apiKey->setPlaceholderText(i18n("Not needed by some local servers"));
    m_apiKey->setClearButtonEnabled(true);

    m_showKey = new QCheckBox(i18n("Show"));
    m_showKey->setObjectName(QStringLiteral("aiChatShowKey"));
    connect(m_showKey, &QCheckBox::toggled, this, [this](bool show) {
        m_apiKey->setEchoMode(show ? QLineEdit::Normal : QLineEdit::Password);
    });

    QHBoxLayout *keyRow = new QHBoxLayout();
    keyRow->setContentsMargins(0, 0, 0, 0);
    keyRow->addWidget(m_apiKey, 1);
    keyRow->addWidget(m_showKey);

    m_model = new QLineEdit();
    m_model->setObjectName(QStringLiteral("aiChatModel"));
    m_model->setPlaceholderText(QStringLiteral("gpt-4o-mini"));
    m_model->setClearButtonEnabled(true);

    m_sendEffort = new QCheckBox(i18n("Send the thinking effort to the server"));
    m_sendEffort->setObjectName(QStringLiteral("aiChatSendEffort"));
    m_sendEffort->setToolTip(i18n("Sends the choice made in the chat panel as \"reasoning_effort\". "
                                  "Leave this off if the server rejects the field."));

    QLabel *keyNote = new QLabel(i18n("The API key is saved in Krita's configuration file, "
                                      "without encryption."));
    keyNote->setWordWrap(true);
    keyNote->setEnabled(false); // dimmed

    QFormLayout *form = new QFormLayout();
    form->addRow(i18n("Server address:"), m_baseUrl);
    form->addRow(QString(), m_urlHint);
    form->addRow(i18n("API key:"), keyRow);
    form->addRow(QString(), keyNote);
    form->addRow(i18n("Model:"), m_model);
    form->addRow(QString(), m_sendEffort);

    // The vision model: lets the AI look at the open image, even if the chat model cannot.
    QLabel *visionTitle = new QLabel(i18n("<b>Vision model</b>"));
    QLabel *visionNote = new QLabel(i18n("Optional. A model that can read pictures, such as DeepSeek Flash with vision. "
                                         "When it is set, the AI can look at the open image and answer questions about it. "
                                         "The image is sent to this server at reduced size. Leave the address or the model "
                                         "empty to switch this off."));
    visionNote->setWordWrap(true);
    visionNote->setEnabled(false); // dimmed

    m_visionUrl = new QLineEdit();
    m_visionUrl->setObjectName(QStringLiteral("aiChatVisionUrl"));
    m_visionUrl->setPlaceholderText(QStringLiteral("https://api.deepseek.com"));
    m_visionUrl->setClearButtonEnabled(true);

    m_visionApiKey = new QLineEdit();
    m_visionApiKey->setObjectName(QStringLiteral("aiChatVisionApiKey"));
    m_visionApiKey->setEchoMode(QLineEdit::Password);
    m_visionApiKey->setClearButtonEnabled(true);
    connect(m_showKey, &QCheckBox::toggled, this, [this](bool show) {
        m_visionApiKey->setEchoMode(show ? QLineEdit::Normal : QLineEdit::Password);
    });

    m_visionModel = new QLineEdit();
    m_visionModel->setObjectName(QStringLiteral("aiChatVisionModel"));
    m_visionModel->setClearButtonEnabled(true);

    form->addRow(QString(), new QLabel()); // space
    form->addRow(visionTitle);
    form->addRow(visionNote);
    form->addRow(i18n("Server address:"), m_visionUrl);
    form->addRow(i18n("API key:"), m_visionApiKey);
    form->addRow(i18n("Model:"), m_visionModel);

    m_visionNoThinking = new QCheckBox(i18n("Ask the vision model not to think"));
    m_visionNoThinking->setObjectName(QStringLiteral("aiChatVisionNoThinking"));
    m_visionNoThinking->setToolTip(i18n("Sends \"reasoning\": {\"enabled\": false}. Thinking makes a look at a picture "
                                        "slow and costs many tokens. Turn this off if the server rejects the field."));
    form->addRow(QString(), m_visionNoThinking);

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addStretch(1);

    connect(m_baseUrl, &QLineEdit::textChanged, this, &AiChatPreferences::slotUrlChanged);

    loadPreferences();
}

QString AiChatPreferences::id()
{
    return QStringLiteral("aichat");
}

QString AiChatPreferences::name()
{
    return i18n("AI Chat");
}

QString AiChatPreferences::header()
{
    return i18n("AI Chat");
}

QIcon AiChatPreferences::icon()
{
    return KisIconUtils::loadIcon("configure");
}

void AiChatPreferences::savePreferences() const
{
    AiChatConfig config;
    config.baseUrl = m_baseUrl->text();
    config.apiKey = m_apiKey->text();
    config.model = m_model->text();
    config.sendReasoningEffort = m_sendEffort->isChecked();
    config.visionBaseUrl = m_visionUrl->text();
    config.visionApiKey = m_visionApiKey->text();
    config.visionModel = m_visionModel->text();
    config.visionDisableThinking = m_visionNoThinking->isChecked();
    config.save();
}

void AiChatPreferences::loadPreferences()
{
    const AiChatConfig config = AiChatConfig::load();
    m_baseUrl->setText(config.baseUrl);
    m_apiKey->setText(config.apiKey);
    m_model->setText(config.model);
    m_sendEffort->setChecked(config.sendReasoningEffort);
    m_visionUrl->setText(config.visionBaseUrl);
    m_visionApiKey->setText(config.visionApiKey);
    m_visionModel->setText(config.visionModel);
    m_visionNoThinking->setChecked(config.visionDisableThinking);
    slotUrlChanged();
}

void AiChatPreferences::loadDefaultPreferences()
{
    const AiChatConfig config = AiChatConfig::defaults();
    m_baseUrl->setText(config.baseUrl);
    m_apiKey->setText(config.apiKey);
    m_model->setText(config.model);
    m_sendEffort->setChecked(config.sendReasoningEffort);
    m_visionUrl->setText(config.visionBaseUrl);
    m_visionApiKey->setText(config.visionApiKey);
    m_visionModel->setText(config.visionModel);
    m_visionNoThinking->setChecked(config.visionDisableThinking);
    slotUrlChanged();
}

void AiChatPreferences::slotUrlChanged()
{
    const QUrl url = AiChatConfig::chatCompletionsUrl(m_baseUrl->text());
    if (m_baseUrl->text().trimmed().isEmpty()) {
        m_urlHint->setText(i18n("The address of an OpenAI compatible server, up to and including \"/v1\"."));
    } else if (!url.isValid()) {
        m_urlHint->setText(i18n("This is not a usable address. It has to start with http:// or https://."));
    } else {
        m_urlHint->setText(i18n("Requests go to %1", url.toString()));
    }
}

KisPreferenceSet *AiChatPreferencesFactory::createPreferenceSet()
{
    return new AiChatPreferences();
}

QString AiChatPreferencesFactory::id() const
{
    return QStringLiteral("aichat");
}
