#include "preferences.hpp"
#include "../debuglog.hpp"

#include "../settings/input_types.hpp"
#include "../settings/settings.hpp"
#include "../settings/settings_singleton.hpp"
#include "buttoninputbox.hpp"
#include "ui_preferences.h"

#include <QCheckBox>
#include <QComboBox>
#include <QRadioButton>
#include <QScrollArea>

#include <QDebug>
#include <QPushButton>
#include <QFrame>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QListWidget>
#include <QTabBar>
#include <QDir>
#include <QFile>
#include <QInputDialog>
#include <QTimer>
#include <QLabel>
#include <QMessageBox>
#include <QSlider>
#include <QStandardPaths>

Preferences::Preferences(QWidget *parent) : QWidget(parent), ui(new Ui::Preferences)
{
	qDebug() << "Initializing Preferences dialog";

	ui->setupUi(this);
	{
		QTabWidget *tabs = ui->preferencesTabWidget;
		auto *sidebar = new QListWidget(this);
		sidebar->setObjectName("prefsSidebar");
		sidebar->setFixedWidth(170);
		sidebar->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		for (int i = 0; i < tabs->count(); ++i)
			sidebar->addItem(tabs->tabText(i));
		sidebar->setCurrentRow(tabs->currentIndex());
		connect(sidebar, &QListWidget::currentRowChanged, tabs, &QTabWidget::setCurrentIndex);
		tabs->tabBar()->hide();
		ui->verticalLayout->removeWidget(tabs);
		auto *row = new QHBoxLayout();
		row->setSpacing(32);
		auto *bodyScroll = new QScrollArea(this);
		bodyScroll->setWidgetResizable(true);
		bodyScroll->setFrameShape(QFrame::NoFrame);
		bodyScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		bodyScroll->setStyleSheet("QScrollArea { background: transparent; border: none; } "
								  "QScrollArea > QWidget > QWidget { background: transparent; }");
		auto fitCurrentPage = [tabs](int current) {
			for (int i = 0; i < tabs->count(); ++i)
			{
				QSizePolicy policy = tabs->widget(i)->sizePolicy();
				policy.setVerticalPolicy(i == current ? QSizePolicy::Preferred
													  : QSizePolicy::Ignored);
				tabs->widget(i)->setSizePolicy(policy);
			}
			tabs->updateGeometry();
		};
		connect(tabs, &QTabWidget::currentChanged, tabs, fitCurrentPage);
		fitCurrentPage(tabs->currentIndex());
		bodyScroll->setWidget(tabs);
		row->addWidget(sidebar, 0, Qt::AlignTop);
		row->addWidget(bodyScroll, 1);
		ui->verticalLayout->insertLayout(0, row, 1);
	}
	setupKeymapTabs();
	ui->pointerSlider->adjustSize();
	ui->pointerSlider->setMaximumWidth(320);
	ui->portSpinBox->setMaximumWidth(140);
	ui->portSpinBox->setRange(1024, 65535);
	ui->portSpinBox->setValue(SettingsSingleton::DEFAULT_PORT_NUMBER);
	ui->portSpinBox->setToolTip(tr("The port the server uses every time it starts (1024-65535)."));
	{
		auto *randomPort = new QCheckBox(tr("Use a different port each time"), this);
		randomPort->setObjectName("randomPortCheck");
		if (auto *row = qobject_cast<QHBoxLayout *>(ui->horizontalLayout_port))
			row->insertWidget(row->indexOf(ui->portSpinBox) + 1, randomPort);
		connect(randomPort, &QCheckBox::toggled, ui->portSpinBox, [this](bool random) { ui->portSpinBox->setEnabled(!random); });
	}
	{
		auto *debug = new QCheckBox(tr("Debug mode: logs everything"), this);
		debug->setObjectName("debugModeCheck");
		debug->setToolTip(tr("Writes every event to debug.log in the app data folder."));
		ui->generalTabLayout->insertWidget(ui->generalTabLayout->indexOf(ui->executorGroup), debug);
		debug->setChecked(SettingsSingleton::instance().qsettings()->value("server/debug", false).toBool());
	}
	{
		auto *several = new QCheckBox(tr("Allow several controllers per device"), this);
		several->setObjectName("multiControllerCheck");
		several->setToolTip(tr("Without it, each phone or browser gets one controller."));
		ui->generalTabLayout->insertWidget(ui->generalTabLayout->indexOf(ui->executorGroup), several);
		several->setChecked(SettingsSingleton::instance().qsettings()->value("server/multi_controller", false).toBool());
	}
	{
		auto *approve = new QCheckBox(tr("Ask before a new device connects"), this);
		approve->setObjectName("requireApprovalCheck");
		approve->setToolTip(tr("A prompt appears on this PC the first time an address connects. Choose Always allow to remember it."));
		ui->generalTabLayout->insertWidget(ui->generalTabLayout->indexOf(ui->executorGroup), approve);
		approve->setChecked(SettingsSingleton::instance().qsettings()->value("server/require_approval", false).toBool());
		auto *forget = new QPushButton(tr("Forget approved devices"), this);
		forget->setObjectName("forgetApprovedButton");
		forget->setMaximumWidth(220);
		ui->generalTabLayout->insertWidget(ui->generalTabLayout->indexOf(ui->executorGroup), forget);
		connect(forget, &QPushButton::clicked, this, [forget]() {
			SettingsSingleton::instance().qsettings()->remove("server/trusted_devices");
			forget->setText(QObject::tr("Forgotten"));
		});
	}
#ifdef _WIN32
	{
		auto *admin = new QCheckBox(tr("Always run as administrator"), this);
		admin->setObjectName("runAsAdminCheck");
		admin->setToolTip(tr("Windows asks for permission each time AirDeck starts. Lets AirDeck clear leftover virtual controllers "
							 "without asking again. Off by default, and it applies the next time AirDeck starts."));
		ui->generalTabLayout->insertWidget(ui->generalTabLayout->indexOf(ui->executorGroup), admin);
		admin->setChecked(SettingsSingleton::instance().qsettings()->value("app/run_as_admin", false).toBool());
	}
#endif
	ui->profileComboBox->setMaximumWidth(320);
	if (auto *profilesRow = qobject_cast<QHBoxLayout *>(ui->profilesGroup->layout()))
		profilesRow->addStretch();
	const auto mappingFields = findChildren<ButtonInputBox *>();
	for (ButtonInputBox *field : mappingFields)
		field->setMaximumWidth(360);
	const auto thresholds = findChildren<QDoubleSpinBox *>();
	for (QDoubleSpinBox *spin : thresholds)
		spin->setMaximumWidth(140);

	if (QPushButton *save = ui->buttonBox->button(QDialogButtonBox::Ok))
		save->setText(tr("Save Changes"));
	ui->buttonBox->setCenterButtons(false);

	// Replace the centered standard button group with a footer that separates the destructive
	// "Restore Defaults" action from the Cancel / Save Changes pair.
	{
		ui->buttonBox->hide();
		auto *footer = new QWidget(this);
		auto *footerLayout = new QHBoxLayout(footer);
		footerLayout->setContentsMargins(0, 0, 0, 0);
		footerLayout->setAlignment(Qt::AlignVCenter);
		footer->setMinimumHeight(60);
		auto *restoreButton = new QPushButton(tr("Restore Defaults"), footer);
		restoreButton->setProperty("variant", "ghost");
		auto *cancelButton = new QPushButton(tr("Cancel"), footer);
		auto *saveButton = new QPushButton(tr("Save Changes"), footer);
		saveButton->setProperty("variant", "primary");
		saveButton->setMinimumWidth(0);
		for (QPushButton *button : {restoreButton, cancelButton, saveButton})
		{
			button->setStyleSheet("font-size: 10pt; padding: 4px 18px; min-height: 20px;");
			button->setFixedHeight(44);
		}
		saveButton->setStyleSheet("font-size: 10pt; font-weight: 600; padding: 12px 22px; min-height: 24px;");
		for (QPushButton *button : {restoreButton, cancelButton, saveButton})
			button->setFixedHeight(50);
		footerLayout->addWidget(restoreButton);
		auto *footerNotice = new QLabel(footer);
		footerNotice->setObjectName("footerStatus");
		footerNotice->setProperty("role", "caption");
		footerLayout->addSpacing(12);
		footerLayout->addWidget(footerNotice);
		footerLayout->addStretch();
		footerLayout->addWidget(cancelButton);
		footerLayout->addWidget(saveButton);
		auto *divider = new QFrame(this);
		divider->setFrameShape(QFrame::HLine);
		ui->verticalLayout->addWidget(divider);
		ui->verticalLayout->addWidget(footer);
		connect(restoreButton, &QPushButton::clicked, this,
				[this] { ui->buttonBox->button(QDialogButtonBox::RestoreDefaults)->click(); });
		connect(cancelButton, &QPushButton::clicked, this,
				[this] { ui->buttonBox->button(QDialogButtonBox::Cancel)->click(); });
		connect(saveButton, &QPushButton::clicked, this,
				[this] { ui->buttonBox->button(QDialogButtonBox::Ok)->click(); });
	}

	connect(ui->buttonBox, &QDialogButtonBox::rejected, this, [this] {
		load_keys();
		emit navigateBack();
		this->deleteLater();
	});
	connect(ui->buttonBox, &QDialogButtonBox::accepted, this, [this] { this->deleteLater(); });
	connect(ui->buttonBox, &QDialogButtonBox::helpRequested, this, &Preferences::show_help);
	connect(ui->buttonBox->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, this,
			&Preferences::restore_defaults);

	ui->buttonBox->setCenterButtons(true);
	ui->pointerSlider->setValue(SettingsSingleton::instance().mouseSensitivity() / 100);
	ui->executorNotesLabel->setOpenExternalLinks(true);
	{
		ui->executorGroup->setMaximumWidth(720);
		ui->gamepadExecutorRadio->setText(tr("InputInjector (Experimental)"));
		auto makeHint = [this](const QString &text) {
			auto *hint = new QLabel(text, ui->executorGroup);
			hint->setProperty("role", "caption");
			hint->setContentsMargins(30, 0, 0, 8);
			return hint;
		};
		ui->executorLayout->removeWidget(ui->executorNotesLabel);
		ui->executorLayout->insertWidget(1, makeHint(tr("Experimental. Needs Administrator and Developer Mode. Can crash on some systems.")));
		ui->executorLayout->insertWidget(3, makeHint(tr("No driver needed. Turns gamepad input into keys and mouse. One phone at a time.")));
#ifdef _WIN32
		auto *vigemRadio = new QRadioButton(tr("Virtual Controller (ViGEm)"), ui->executorGroup);
		vigemRadio->setObjectName("vigemExecutorRadio");
		ui->executorLayout->insertWidget(0, vigemRadio);
		ui->executorLayout->insertWidget(
			1, makeHint(tr("Recommended for most games. Each phone is a real controller that games see as an Xbox 360 or a PlayStation pad. Needs the ViGEmBus driver.")));
		connect(vigemRadio, &QRadioButton::toggled, this, &Preferences::executor_type_changed);
		{
			// Which controller games think they see
			auto *row = new QWidget(ui->executorGroup);
			auto *rowLayout = new QHBoxLayout(row);
			rowLayout->setContentsMargins(30, 0, 0, 0);
			rowLayout->addWidget(new QLabel(tr("Controller type"), row));
			auto *style = new QComboBox(row);
			style->setObjectName("padStyleCombo");
			style->addItem(tr("Xbox 360 (XInput)"), QStringLiteral("xbox"));
			style->addItem(tr("PlayStation (DualShock 4)"), QStringLiteral("playstation"));
			style->setMinimumWidth(240);
			style->setToolTip(tr("How games detect the virtual controller. PlayStation shows PlayStation buttons in games that support them. "
								 "Games that only read XInput may not see a PlayStation pad. Applies to devices that connect after you save."));
			const int at = style->findData(SettingsSingleton::instance().qsettings()->value("server/pad_style", "xbox").toString());
			style->setCurrentIndex(at >= 0 ? at : 0);
			rowLayout->addWidget(style);
			rowLayout->addStretch();
			ui->executorLayout->insertWidget(2, row);
			ui->executorLayout->insertWidget(3, makeHint(tr("Games with PlayStation support show PlayStation button prompts. XInput-only games may not see a PlayStation pad.")));
			style->setEnabled(vigemRadio->isChecked());
			connect(vigemRadio, &QRadioButton::toggled, style, &QWidget::setEnabled);
		}
#endif
		auto *card = new QFrame(ui->executorGroup);
		card->setStyleSheet("QFrame { background-color: #1A1A1A; border: none; border-radius: 4px; }");
		auto *cardLayout = new QVBoxLayout(card);
		cardLayout->setContentsMargins(16, 14, 16, 14);
		auto *cardTitle = new QLabel(card);
		cardTitle->setObjectName("executorDetailsTitle");
		cardTitle->setStyleSheet("font-weight: 600; font-size: 11pt; background: transparent;");
		cardLayout->addWidget(cardTitle);
		ui->executorNotesLabel->setParent(card);
		ui->executorNotesLabel->setMinimumHeight(130);
		cardLayout->addWidget(ui->executorNotesLabel);
		ui->executorLayout->addWidget(card);
	}

	setup_profile_management();
	ui->deleteProfileButton->setEnabled(ui->profileComboBox->currentText() != "Default");

	// Load preferences into UI
	load_port();
	load_executor_type();

	// Connect executor type radio buttons
	connect(ui->gamepadExecutorRadio, &QRadioButton::toggled, this,
			&Preferences::executor_type_changed);
	connect(ui->keyboardMouseExecutorRadio, &QRadioButton::toggled, this,
			&Preferences::executor_type_changed);

	// The keymap only applies when keys and mouse are simulated
	{
		auto *sidebar = findChild<QListWidget *>("prefsSidebar");
		auto applyKeymapState = [this, sidebar]() {
			if (sidebar == nullptr)
				return;
			const bool enabled = ui->keyboardMouseExecutorRadio->isChecked();
			for (int i = 0; i < sidebar->count(); ++i)
			{
				QListWidgetItem *item = sidebar->item(i);
				if (item->text() != tr("Keymap") && item->text() != QStringLiteral("Keymap"))
					continue;
				item->setFlags(enabled ? (item->flags() | Qt::ItemIsEnabled) : (item->flags() & ~Qt::ItemIsEnabled));
				item->setToolTip(enabled ? QString() : tr("Only used in Keyboard/Mouse mode"));
				item->setForeground(enabled ? QBrush() : QBrush(QColor("#5A5A60")));
				if (!enabled && sidebar->currentRow() == i)
					sidebar->setCurrentRow(0);
			}
		};
		connect(ui->keyboardMouseExecutorRadio, &QRadioButton::toggled, this, applyKeymapState);
		applyKeymapState();
	}

	// No immediate save connections - only save when OK is clicked
	disconnect(ui->buttonBox, &QDialogButtonBox::accepted, nullptr, nullptr);
	connect(ui->buttonBox, &QDialogButtonBox::accepted, this, [this] {
		auto &settings = SettingsSingleton::instance();

		// Save mouse sensitivity
		settings.setMouseSensitivity(ui->pointerSlider->value() * 100);

		if (auto *debug = findChild<QCheckBox *>("debugModeCheck"))
		{
			settings.qsettings()->setValue("server/debug", debug->isChecked());
			DebugLog::write(debug->isChecked() ? QStringLiteral("Debug mode switched on") : QStringLiteral("Debug mode switching off"), true);
			DebugLog::setEnabled(debug->isChecked());
		}
		if (auto *several = findChild<QCheckBox *>("multiControllerCheck"))
			settings.qsettings()->setValue("server/multi_controller", several->isChecked());
		if (auto *approve = findChild<QCheckBox *>("requireApprovalCheck"))
			settings.qsettings()->setValue("server/require_approval", approve->isChecked());
		if (auto *style = findChild<QComboBox *>("padStyleCombo"))
			settings.qsettings()->setValue("server/pad_style", style->currentData().toString());
		if (auto *admin = findChild<QCheckBox *>("runAsAdminCheck"))
			settings.qsettings()->setValue("app/run_as_admin", admin->isChecked());

		// Save port number now
		auto *randomPort = findChild<QCheckBox *>("randomPortCheck");
		settings.setPort(randomPort != nullptr && randomPort->isChecked() ? 0 : ui->portSpinBox->value());

		// Save executor type
		ExecutorType executorType = ui->gamepadExecutorRadio->isChecked()
										? ExecutorType::GamepadExecutor
										: ExecutorType::KeyboardMouseExecutor;
		if (auto *vigemRadio = findChild<QRadioButton *>("vigemExecutorRadio");
			vigemRadio != nullptr && vigemRadio->isChecked())
			executorType = ExecutorType::VigemExecutor;
		settings.setExecutorType(executorType);

		// Update key mapping in the active profile
		change_key_inputs();

		// Update and save the active profile
		currentProfile = ui->profileComboBox->currentText();
		settings.setActiveProfileName(currentProfile);
		settings.saveActiveProfile();

		emit saved();
		emit navigateBack();
		this->deleteLater();
	});
}

Preferences::~Preferences()
{
	delete ui;
}

void Preferences::setupKeymapTabs()
{
	// This function can be used to connect signals/slots for the new tab structure if needed.
}

void Preferences::setup_profile_management()
{
	auto const &settings = SettingsSingleton::instance();

	// Connect buttons
	connect(ui->newProfileButton, &QPushButton::clicked, this, &Preferences::new_profile);
	connect(ui->deleteProfileButton, &QPushButton::clicked, this, &Preferences::delete_profile);
	connect(ui->profileComboBox, &QComboBox::currentTextChanged, this,
			&Preferences::profile_selection_changed);

	// Initialize with available profiles
	refresh_profile_list();

	// Set active profile
	currentProfile = settings.activeProfileName();
	int currentProfileIndex =
		ui->profileComboBox->findText(currentProfile.isEmpty() ? "Default" : currentProfile);
	if (currentProfileIndex >= 0)
	{
		ui->profileComboBox->setCurrentIndex(currentProfileIndex);
	}

	// Explicitly trigger the profile selection change to load the profile
	profile_selection_changed(ui->profileComboBox->currentText());
}

void Preferences::refresh_profile_list()
{
	ui->profileComboBox->blockSignals(true);
	ui->profileComboBox->clear();

	QStringList profiles = SettingsSingleton::instance().listAvailableProfiles();
	ui->profileComboBox->addItems(profiles);

	ui->profileComboBox->blockSignals(false);
}

void Preferences::new_profile()
{
	QInputDialog dialog(this);
	dialog.setWindowTitle(tr("New Profile"));
	dialog.setLabelText(tr("Profile name"));
	dialog.setOkButtonText(tr("Create Profile"));
	dialog.setCancelButtonText(tr("Cancel"));
	dialog.setMinimumWidth(400);
	const bool ok = dialog.exec() == QDialog::Accepted;
	const QString profileName = dialog.textValue().trimmed();
	auto &settings = SettingsSingleton::instance();

	if (ok && !profileName.isEmpty())
	{
		qInfo() << "Creating new profile:" << profileName;

		// Check if profile already exists
		if (settings.profileExists(profileName))
		{
			qWarning() << "Profile creation failed - profile already exists:" << profileName;
			QMessageBox::warning(
				this, "Profile Exists",
				"A profile with this name already exists. Please choose a different name.");
			return;
		}

		// Ensure current UI values are applied to the active profile
		change_key_inputs();

		// Save current settings as new profile
		if (settings.createProfile(profileName))
		{
			qInfo() << "Profile created successfully:" << profileName;
			refresh_profile_list();
			ui->profileComboBox->setCurrentText(profileName);
			currentProfile = profileName;

		}
		else
		{
			qCritical() << "Failed to create profile:" << profileName;
			QMessageBox::warning(this, "Error", "Failed to create profile '" + profileName + "'");
		}
	}
}

void Preferences::delete_profile()
{
	QString profileName = ui->profileComboBox->currentText();
	qInfo() << "Attempting to delete profile:" << profileName;

	if (profileName.isEmpty())
	{
		return;
	}

	auto &settings = SettingsSingleton::instance();

	// The built-in Default profile is never deleted (the button is disabled for it)
	if (profileName == "Default")
		return;
	
	QMessageBox confirm(this);
	confirm.setWindowTitle(tr("Delete profile"));
	confirm.setIcon(QMessageBox::Question);
	confirm.setText(tr("Delete the profile \"%1\"?").arg(profileName));
	confirm.setInformativeText(profileName == settings.activeProfileName()
			? tr("It is the active profile, so Default becomes active. This cannot be undone.")
			: tr("This cannot be undone."));
	auto *deleteButton = confirm.addButton(tr("Delete Profile"), QMessageBox::DestructiveRole);
	confirm.addButton(tr("Cancel"), QMessageBox::RejectRole);
	confirm.exec();
	if (confirm.clickedButton() != deleteButton)
		return;
	
	if (profileName == settings.activeProfileName())
		ui->profileComboBox->setCurrentText("Default");
	
	if (settings.deleteProfile(profileName))
	{
		qInfo() << "Profile deleted successfully:" << profileName;
		refresh_profile_list();
		ui->profileComboBox->setCurrentText(settings.activeProfileName());
	}
	else
	{
		qCritical() << "Failed to delete profile:" << profileName;
		QMessageBox::warning(this, "Error", "Failed to delete profile '" + profileName + "'");
	}
	}
	
void Preferences::profile_selection_changed(const QString &profileName)
{
	if (profileName.isEmpty())
		return;

	ui->deleteProfileButton->setEnabled(profileName != "Default");
	ui->deleteProfileButton->setToolTip(profileName == "Default" ? tr("The Default profile cannot be deleted")
																: QString());
	qDebug() << "Profile selection changed to:" << profileName;

	auto &settings = SettingsSingleton::instance();

	// Discard any changes to the current profile (don't save)
	// Just load the new profile directly

	// Load the selected profile using SettingsSingleton
	if (settings.loadProfile(profileName))
	{
		qInfo() << "Profile loaded successfully:" << profileName;
		currentProfile = profileName;
		// Update UI from the loaded profile's keymaps
		load_keys();
	}
	else
	{
		qCritical() << "Failed to load profile:" << profileName;
		QMessageBox::warning(this, "Error", "Failed to load profile '" + profileName + "'");
	}
}

void Preferences::load_thumbsticks()
{
	auto const &profile = SettingsSingleton::instance().activeKeymapProfile();
	// Left thumbstick
	ui->leftThumbMouseMove->setChecked(profile.leftThumbMouseMove());
	auto left = profile.thumbstickInput(Thumbstick_Left);
	ui->leftThumbUpMap->setKeyCodeAndDisplayName(left.up.vk, left.up.displayName);
	ui->leftThumbDownMap->setKeyCodeAndDisplayName(left.down.vk, left.down.displayName);
	ui->leftThumbLeftMap->setKeyCodeAndDisplayName(left.left.vk, left.left.displayName);
	ui->leftThumbRightMap->setKeyCodeAndDisplayName(left.right.vk, left.right.displayName);

	// Right thumbstick
	ui->rightThumbMouseMove->setChecked(profile.rightThumbMouseMove());
	auto right = profile.thumbstickInput(Thumbstick_Right);
	ui->rightThumbUpMap->setKeyCodeAndDisplayName(right.up.vk, right.up.displayName);
	ui->rightThumbDownMap->setKeyCodeAndDisplayName(right.down.vk, right.down.displayName);
	ui->rightThumbLeftMap->setKeyCodeAndDisplayName(right.left.vk, right.left.displayName);
	ui->rightThumbRightMap->setKeyCodeAndDisplayName(right.right.vk, right.right.displayName);
}

void Preferences::load_triggers()
{
	auto const &profile = SettingsSingleton::instance().activeKeymapProfile();

	// Left trigger
	auto leftTrigger = profile.triggerInput(Trigger::Left);
	ui->leftTriggerButtonMap->setKeyCodeAndDisplayName(leftTrigger.button_input.vk,
													   leftTrigger.button_input.displayName);
	ui->leftTriggerThreshold->setValue(leftTrigger.threshold *
									   100.0); // Convert from [0.0-1.0] to [0-100]%

	// Right trigger
	auto rightTrigger = profile.triggerInput(Trigger::Right);
	ui->rightTriggerButtonMap->setKeyCodeAndDisplayName(rightTrigger.button_input.vk,
														rightTrigger.button_input.displayName);
	ui->rightTriggerThreshold->setValue(rightTrigger.threshold *
										100.0); // Convert from [0.0-1.0] to [0-100]%
}

void Preferences::change_key_inputs()
{
	auto &profile = SettingsSingleton::instance().activeKeymapProfile();
	auto getBox = [&](ButtonInputBox const *box, GamepadButtons btn) {
		InputKeyCode vk = box->keyCode();
		QString displayName = box->displayName();
		profile.setButtonInput(btn, vk, displayName);
	};
	getBox(ui->xmap, GamepadButtons_X);
	getBox(ui->ymap, GamepadButtons_Y);
	getBox(ui->amap, GamepadButtons_A);
	getBox(ui->bmap, GamepadButtons_B);
	getBox(ui->Ltmap, GamepadButtons_LeftShoulder);
	getBox(ui->Rtmap, GamepadButtons_RightShoulder);
	getBox(ui->ddownmap, GamepadButtons_DPadDown);
	getBox(ui->dupmap, GamepadButtons_DPadUp);
	getBox(ui->drightmap, GamepadButtons_DPadRight);
	getBox(ui->dleftmap, GamepadButtons_DPadLeft);
	getBox(ui->viewmap, GamepadButtons_View);
	getBox(ui->menumap, GamepadButtons_Menu);
	getBox(ui->lthumbmap, GamepadButtons_LeftThumbstick);
	getBox(ui->rthumbmap, GamepadButtons_RightThumbstick);
	change_thumbstick_inputs();
	change_trigger_inputs();
}

void Preferences::change_thumbstick_inputs()
{
	auto &profile = SettingsSingleton::instance().activeKeymapProfile();
	// Left thumbstick
	ThumbstickInput left;
	left.is_mouse_move = ui->leftThumbMouseMove->isChecked();
	left.up.vk = ui->leftThumbUpMap->keyCode();
	left.up.displayName = ui->leftThumbUpMap->displayName();
	left.down.vk = ui->leftThumbDownMap->keyCode();
	left.down.displayName = ui->leftThumbDownMap->displayName();
	left.left.vk = ui->leftThumbLeftMap->keyCode();
	left.left.displayName = ui->leftThumbLeftMap->displayName();
	left.right.vk = ui->leftThumbRightMap->keyCode();
	left.right.displayName = ui->leftThumbRightMap->displayName();
	profile.setThumbstickInput(Thumbstick_Left, left);

	// Right thumbstick
	ThumbstickInput right;
	right.is_mouse_move = ui->rightThumbMouseMove->isChecked();
	right.up.vk = ui->rightThumbUpMap->keyCode();
	right.up.displayName = ui->rightThumbUpMap->displayName();
	right.down.vk = ui->rightThumbDownMap->keyCode();
	right.down.displayName = ui->rightThumbDownMap->displayName();
	right.left.vk = ui->rightThumbLeftMap->keyCode();
	right.left.displayName = ui->rightThumbLeftMap->displayName();
	right.right.vk = ui->rightThumbRightMap->keyCode();
	right.right.displayName = ui->rightThumbRightMap->displayName();
	profile.setThumbstickInput(Thumbstick_Right, right);
}

void Preferences::change_trigger_inputs()
{
	auto &profile = SettingsSingleton::instance().activeKeymapProfile();

	// Left trigger
	TriggerInput leftTrigger;
	leftTrigger.button_input.vk = ui->leftTriggerButtonMap->keyCode();
	leftTrigger.button_input.displayName = ui->leftTriggerButtonMap->displayName();
	leftTrigger.button_input.is_mouse_button = is_mouse_button(leftTrigger.button_input.vk);
	// Convert from [0-100]% to [0.0-1.0]
	leftTrigger.threshold = static_cast<float>(ui->leftTriggerThreshold->value() / 100.0);
	profile.setTriggerInput(Trigger::Left, leftTrigger);

	// Right trigger
	TriggerInput rightTrigger;
	rightTrigger.button_input.vk = ui->rightTriggerButtonMap->keyCode();
	rightTrigger.button_input.displayName = ui->rightTriggerButtonMap->displayName();
	rightTrigger.button_input.is_mouse_button = is_mouse_button(rightTrigger.button_input.vk);
	// Convert from [0-100]% to [0.0-1.0]
	rightTrigger.threshold = static_cast<float>(ui->rightTriggerThreshold->value() / 100.0);
	profile.setTriggerInput(Trigger::Right, rightTrigger);
}

/**
 * Displays the key to which each button is mapped to.
 * Saves the initial key maps in variables that can be changed later if user wants to.
 */
void Preferences::load_keys()
{
	auto const &profile = SettingsSingleton::instance().activeKeymapProfile();
	auto setBox = [&](ButtonInputBox *box, GamepadButtons btn) {
		box->setKeyCodeAndDisplayName(profile.buttonMap(btn), profile.buttonDisplayName(btn));
	};
	setBox(ui->xmap, GamepadButtons_X);
	setBox(ui->ymap, GamepadButtons_Y);
	setBox(ui->amap, GamepadButtons_A);
	setBox(ui->bmap, GamepadButtons_B);
	setBox(ui->Rtmap, GamepadButtons_RightShoulder);
	setBox(ui->Ltmap, GamepadButtons_LeftShoulder);
	setBox(ui->ddownmap, GamepadButtons_DPadDown);
	setBox(ui->dupmap, GamepadButtons_DPadUp);
	setBox(ui->drightmap, GamepadButtons_DPadRight);
	setBox(ui->dleftmap, GamepadButtons_DPadLeft);
	setBox(ui->viewmap, GamepadButtons_View);
	setBox(ui->menumap, GamepadButtons_Menu);
	setBox(ui->lthumbmap, GamepadButtons_LeftThumbstick);
	setBox(ui->rthumbmap, GamepadButtons_RightThumbstick);
	load_thumbsticks();
	load_triggers();
}

void Preferences::change_mouse_sensitivity(int value)
{
	SettingsSingleton::instance().setMouseSensitivity(value);
}

void Preferences::show_help()
{
	QString helpText = tr(R"(
Changes are saved to disk only when you press Ok.  
Restore Defaults loads defaults for all settings (including the active profile).

The settings file is located at:  
[`%1`](file:///%1)

You can switch between different keymaps for different games using Profiles.  
You can share profiles with others (same OS only) by copying the files in the Profiles directory at:  
[`%2`](file:///%2)
)")
						   .arg(SettingsSingleton::instance().qsettings()->fileName())
						   .arg(SettingsSingleton::instance().getProfilesDir());

	QMessageBox helpBox(this);
	helpBox.setWindowTitle("Preferences Help");
	helpBox.setIcon(QMessageBox::Icon::Information);

	helpBox.setText(helpText);
	helpBox.setTextFormat(Qt::TextFormat::MarkdownText);
	helpBox.setStandardButtons(QMessageBox::Ok);
	helpBox.exec();
}

void Preferences::load_port()
{
	const int port = SettingsSingleton::instance().port();
	const bool random = port < 1024 || port > 65535;
	ui->portSpinBox->setValue(random ? SettingsSingleton::DEFAULT_PORT_NUMBER : port);
	ui->portSpinBox->setEnabled(!random);
	if (auto *check = findChild<QCheckBox *>("randomPortCheck"))
		check->setChecked(random);
}

void Preferences::change_port(int value)
{
	SettingsSingleton::instance().setPort(value);
}

void Preferences::restore_defaults()
{
	QMessageBox confirm(this);
	confirm.setWindowTitle(tr("Restore defaults"));
	confirm.setIcon(QMessageBox::Question);
	confirm.setText(tr("Restore all settings to their defaults?"));
	confirm.setInformativeText(tr("This resets the port, input mode, pointer speed and the active key mapping."));
	auto *restoreButton = confirm.addButton(tr("Restore Defaults"), QMessageBox::DestructiveRole);
	confirm.addButton(tr("Cancel"), QMessageBox::RejectRole);
	confirm.exec();
	if (confirm.clickedButton() != restoreButton)
		return;
	
	// Delegate to SettingsSingleton to reset the settings
	auto &settings = SettingsSingleton::instance();
	settings.resetToDefaults();

	// Update UI to reflect the default values
	ui->pointerSlider->setValue(SettingsSingleton::DEFAULT_MOUSE_SENSITIVITY);
	ui->portSpinBox->setValue(SettingsSingleton::DEFAULT_PORT_NUMBER);
	if (SettingsSingleton::DEFAULT_EXECUTOR_TYPE == ExecutorType::GamepadExecutor)
	{
		ui->gamepadExecutorRadio->setChecked(true);
	}
	else
	{
		ui->keyboardMouseExecutorRadio->setChecked(true);
	}

	if (auto *debug = findChild<QCheckBox *>("debugModeCheck"))
		debug->setChecked(false);
	settings.qsettings()->setValue("server/debug", false);
	if (auto *several = findChild<QCheckBox *>("multiControllerCheck"))
		several->setChecked(false);
	settings.qsettings()->setValue("server/multi_controller", false);
	if (auto *style = findChild<QComboBox *>("padStyleCombo"))
		style->setCurrentIndex(0);
	settings.qsettings()->setValue("server/pad_style", "xbox");
	if (auto *approve = findChild<QCheckBox *>("requireApprovalCheck"))
		approve->setChecked(false);
	settings.qsettings()->setValue("server/require_approval", false);
	settings.qsettings()->remove("server/trusted_devices");
	if (auto *admin = findChild<QCheckBox *>("runAsAdminCheck"))
		admin->setChecked(false);
	settings.qsettings()->setValue("app/run_as_admin", false);

	// Refresh UI to show the default values
	load_keys();
	load_port();
	load_executor_type();

	if (auto *notice = findChild<QLabel *>("footerStatus"))
	{
		notice->setText(tr("Defaults loaded. Save Changes to keep them."));
		QTimer::singleShot(6000, notice, [notice]() { notice->clear(); });
	}
}

void Preferences::load_executor_type()
{
	ExecutorType executorType = SettingsSingleton::instance().executorType();

	auto *vigemRadio = findChild<QRadioButton *>("vigemExecutorRadio");
	if (executorType == ExecutorType::VigemExecutor && vigemRadio != nullptr)
	{
		vigemRadio->setChecked(true);
	}
	else if (executorType == ExecutorType::GamepadExecutor)
	{
		ui->gamepadExecutorRadio->setChecked(true);
	}
	else
	{
		ui->keyboardMouseExecutorRadio->setChecked(true);
	}

	update_executor_notes();
}

void Preferences::executor_type_changed()
{
	update_executor_notes();
}

void Preferences::update_executor_notes()
{
	QString notes;
	auto linux_common_notes =
		"Requires access to /dev/uinput. "
		"uinput kernel module must be loaded.\n"
		"\nQuick and dirty access (not recommended for security):\n"
		"- run the application with sudo\n"
		"- Or, `sudo chmod 666 /dev/uinput`\n"
		"\nRecommended way:\n"
		"1. Create the uinput group if it doesn't exist: sudo groupadd uinput\n"
		"2. Add your user to the uinput group: sudo usermod -aG uinput $USER\n"
		"3. Make it persistent with this command:\n"
		" ``` sudo sh -c 'echo KERNEL==\\\"uinput\\\", SUBSYSTEM==\\\"misc\\\", MODE=\\\"0660\\\", "
		"GROUP=\\\"uinput\\\" > /etc/udev/rules.d/99-uinput.rules' ```\n"
		"4. Log out and back in for the group changes to take effect.";

#if defined(__linux__)
	// Test for access
	if (access("/dev/uinput", R_OK | W_OK) != 0)
	{
		notes = linux_common_notes;
	}
	else
	{
		notes = "RW access to /dev/uinput is available on this system. No action required.";
	}
#endif

	auto *vigemRadio = findChild<QRadioButton *>("vigemExecutorRadio");
	const bool vigemSelected = vigemRadio != nullptr && vigemRadio->isChecked();
	if (vigemSelected)
	{
		notes = "- Windows 10 or later\n"
				"- Needs the [ViGEmBus driver](https://github.com/nefarius/ViGEmBus/releases) "
				"installed once\n"
				"- Each phone becomes its own controller (up to 8), seen as an Xbox 360 or a DualShock 4\n"
				"- Xbox 360 works with any game that supports XInput. PlayStation shows PlayStation buttons in games that support it";
	}
	else if (ui->gamepadExecutorRadio->isChecked())
	{
		// notes for GamepadExecutor
#ifdef _WIN32
		notes = "- Windows 10 or later\n"
				"- Administrator privileges\n"
				"- Enable [app sideloading (developer mode) in Settings](ms-settings:developers)\n"
				"- Creates a virtual gamepad device\n"
				"- **Experimental**: "
				"Known to crash at server launch. [View known "
				"issue.](https://github.com/microsoft/microsoft-ui-xaml/issues/8639)";
#elif defined(__linux__)
#else
		notes = "Platform-specific notes apply for virtual gamepad creation.";
#endif
	}
	else
	{
		// notes for KeyboardMouseExecutor
#ifdef _WIN32
		notes = "- Windows 7 or later\n"
				"- No special privileges required\n"
				"- Simulates keyboard and mouse input\n"
				"- Uses configurable key mappings";
#elif defined(__linux__)
#else
		notes = "Simulates keyboard and mouse input using configurable key mappings.";
#endif
	}

	ui->executorNotesLabel->setText(notes);
	if (auto *title = findChild<QLabel *>("executorDetailsTitle"))
		title->setText(vigemSelected							  ? tr("Virtual Controller (Xbox 360 or PlayStation)")
					   : ui->gamepadExecutorRadio->isChecked() ? tr("InputInjector")
															   : tr("Keyboard/Mouse Simulation"));
}
