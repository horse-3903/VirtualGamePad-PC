#include "server.hpp"
#include "../debuglog.hpp"

#include "../../third-party-libs/QR-Code-generator/cpp/qrcodegen.hpp"
#include "../platform/elevation.hpp"
#include "../ui/glyphs.hpp"
#include "../ui/slotcolors.hpp"
#include "../settings/settings_singleton.hpp"
#include "ui_server.h"

#include <QPointer>
#include "../platform/padcleanup.hpp"
#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QStandardPaths>
#include <QTextStream>
#include <QButtonGroup>
#include <QGridLayout>
#include <QClipboard>
#include <QCryptographicHash>
#include <QFile>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDataStream>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QList>
#include <QMessageBox>
#include <QNetworkInterface>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QSvgRenderer>
#include <functional>
#include <QThread>
#include <memory>
#include <thread>
#include <QTimer>
#include <algorithm>


#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
// Creating the Windows virtual gamepad can hit a native access violation on some systems
// (see https://github.com/microsoft/microsoft-ui-xaml/issues/8639). C++ exceptions cannot catch
// it, so creation is wrapped in structured exception handling to keep the server alive.
static GamepadExecutor *createGamepadCpp()
{
	try
	{
		return new GamepadExecutor();
	}
	catch (...)
	{
		return nullptr;
	}
}

static GamepadExecutor *createGamepadGuarded(bool *crashed)
{
	GamepadExecutor *result = nullptr;
	__try
	{
		result = createGamepadCpp();
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		*crashed = true;
		result = nullptr;
	}
	return result;
}
#else
static GamepadExecutor *createGamepadGuarded(bool *crashed)
{
	try
	{
		return new GamepadExecutor();
	}
	catch (...)
	{
		return nullptr;
	}
}
#endif

/**
 * @brief Creates a QR code from a string
 *
 * This function uses [Nayuki's QR Code Generator
 * library](https://github.com/nayuki/QR-Code-generator/tree/master/cpp).\n Guided by:
 * https://stackoverflow.com/a/39951669/8659747
 * @param data The string to encode
 * @param border The padding around the QR code (in pixels)
 * @param scalingFactor The scaling factor for the final image
 * @return QImage
 */
QImage createQR(const QString &data, const int border = 1, const uint scalingFactor = 8)
{
	char *str = data.toUtf8().data();
	qrcodegen::QrCode qr = qrcodegen::QrCode::encodeText(str, qrcodegen::QrCode::Ecc::HIGH);
	const int s = qr.getSize(); // s is the length of a side of the QR code
	qDebug() << "QR code generated with size: " << s;
	QImage image(s + 2 * border, s + 2 * border, QImage::Format_Mono);
	image.setColor(1, QColor("black").rgb());
	image.setColor(0, QColor("white").rgb());
	image.fill(0); // Whitewash
	for (int y = 0; y < s; y++)
	{
		for (int x = 0; x < s; x++)
		{
			const int color = qr.getModule(x, y); // 0 for white, 1 for black
			image.setPixel(x + border, y + border, color);
		}
	}
	return image.scaled(image.size() * scalingFactor);
}

namespace
{
const char *kHome = "<path d='M3 11l9-8 9 8'/><path d='M5 10v10h5v-6h4v6h5V10'/>";
const char *kRestart = "<path d='M20 12a8 8 0 1 1-2.3-5.6'/><path d='M20 4v5h-5'/>";
const char *kStop = "<rect x='5' y='5' width='14' height='14' rx='3'/>";
const char *kLink = "<path d='M10 14a4 4 0 0 0 5.7 0l3-3a4 4 0 0 0-5.7-5.7l-1 1'/><path d='M14 10a4 4 0 0 0-5.7 0l-3 3a4 4 0 0 0 5.7 5.7l1-1'/>";
const char *kGrid = "<rect x='4' y='4' width='6.5' height='6.5' rx='1.6'/><rect x='13.5' y='4' width='6.5' height='6.5' rx='1.6'/>"
					"<rect x='4' y='13.5' width='6.5' height='6.5' rx='1.6'/><rect x='13.5' y='13.5' width='6.5' height='6.5' rx='1.6'/>";
const char *kServer = "<rect x='3' y='4' width='18' height='6.5' rx='2'/><rect x='3' y='13.5' width='18' height='6.5' rx='2'/><path d='M7 7.25h.01M7 16.75h.01'/>";
const char *kActivity = "<path d='M3 12h4l3-8 4 16 3-8h4'/>";

QPixmap svgPixmap(const QString &body, const QColor &color, int size, qreal dpr)
{
	const QString svg = QStringLiteral("<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='%1' "
									   "stroke-width='2' stroke-linecap='round' stroke-linejoin='round'>%2</svg>")
							.arg(color.name(), body);
	QSvgRenderer renderer(svg.toUtf8());
	QPixmap pixmap(QSize(size, size) * dpr);
	pixmap.setDevicePixelRatio(dpr);
	pixmap.fill(Qt::transparent);
	QPainter painter(&pixmap);
	renderer.render(&painter, QRectF(0, 0, size, size));
	return pixmap;
}
} // namespace

Server::Server(QWidget *parent) : QWidget(parent), ui(new Ui::Server)
{
	qInfo() << "Initializing TCP server";

	ui->setupUi(this);
	ui->IPList->viewport()->setAutoFillBackground(false);
	// delete this when stop button is clicked
	connect(ui->stopButton, &QPushButton::clicked, this, &Server::stopServer);
	{
		const qreal dpr = devicePixelRatioF();
		const QString neutral = "QPushButton { background: #17171B; border: 1px solid #2C2C33; border-radius: 10px; padding: 0; } "
								"QPushButton:hover { background: #202026; border-color: #3A3A43; } "
								"QPushButton:pressed { background: #2A2A31; } "
								"QPushButton:focus { border-color: #4A4A55; outline: none; } "
								"QPushButton:disabled { background: #121215; border-color: #222227; }";
		const QString danger = "QPushButton { background: #2A1517; border: 1px solid #5A2A2F; border-radius: 10px; padding: 0; } "
							   "QPushButton:hover { background: #381B1E; border-color: #7A3A40; } "
							   "QPushButton:pressed { background: #44212A; } "
							   "QPushButton:focus { border-color: #8A4A50; outline: none; } "
							   "QPushButton:disabled { background: #1A1112; border-color: #3A2124; }";
		const auto setup = [&](QPushButton *button, const char *body, const QColor &muted, const QColor &bright, const QString &tip,
							   const QString &style) {
			QIcon icon;
			icon.addPixmap(svgPixmap(body, muted, 20, dpr), QIcon::Normal);
			icon.addPixmap(svgPixmap(body, bright, 20, dpr), QIcon::Active);
			button->setIcon(icon);
			button->setIconSize(QSize(20, 20));
			button->setText(QString());
			button->setFixedSize(40, 40);
			button->setCursor(Qt::PointingHandCursor);
			button->setToolTip(tip);
			button->setAccessibleName(tip);
			button->setStyleSheet(style);
		};
		homeBtn = new QPushButton(this);
		restartBtn = new QPushButton(this);
		setup(homeBtn, kHome, QColor("#A8A8B0"), QColor("#F2F2F2"), tr("Back to Home"), neutral);
		setup(restartBtn, kRestart, QColor("#A8A8B0"), QColor("#F2F2F2"), tr("Restart server"), neutral);
		setup(ui->stopButton, kStop, QColor("#D9737B"), QColor("#FF9AA2"), tr("Stop server"), danger);
		ui->headerLayout->removeWidget(ui->stopButton);
		auto *actions = new QHBoxLayout();
		actions->setSpacing(8);
		actions->addWidget(homeBtn);
		actions->addWidget(restartBtn);
		actions->addWidget(ui->stopButton);
		ui->headerLayout->addLayout(actions);
		connect(homeBtn, &QPushButton::clicked, this, &Server::homeRequested);
		connect(restartBtn, &QPushButton::clicked, this, &Server::restartServer);
	}
	{
		const ExecutorType mode = SettingsSingleton::instance().executorType();
		const QString modeName =
			mode == ExecutorType::VigemExecutor		? tr("Xbox 360 (ViGEm)")
			: mode == ExecutorType::GamepadExecutor ? tr("Windows virtual gamepad")
													: tr("Keyboard and mouse");
		const QString pillStyle =
			"QLabel { background: #161616; border: 1px solid %1; border-radius: 4px; "
			"padding: 4px 12px; color: %2; font-weight: 600; }";
		serverMode = mode;
		auto *pillRow = new QHBoxLayout();
		pillRow->setSpacing(8);
		countPill = new QLabel(this);
		countPill->setProperty("role", "caption");
		pillRow->addWidget(countPill);
		if (isElevated())
		{
			auto *adminPill = new QLabel(tr("Administrator"), this);
			adminPill->setStyleSheet(pillStyle.arg("#8A6A1F", "#F2C14E"));
			pillRow->addWidget(adminPill);
		}
		pillRow->addStretch();
		auto *titleColumn = new QVBoxLayout();
		titleColumn->setSpacing(6);
		ui->headerLayout->removeWidget(ui->statusLabel);
		auto *statusRow = new QHBoxLayout();
		statusRow->setSpacing(8);
		statusRow->addWidget(ui->statusLabel, 1);
		titleColumn->addLayout(statusRow);
		titleColumn->addLayout(pillRow);
		ui->headerLayout->insertLayout(0, titleColumn);
		serverIcon = new QLabel(this);
		serverIcon->setFixedSize(48, 48);
		serverIcon->setAlignment(Qt::AlignCenter);
		ui->headerLayout->insertWidget(0, serverIcon, 0, Qt::AlignTop);
		ui->headerLayout->insertSpacing(1, 6);
		setServerState(0);
	}

	// Two views on one page: Controllers (who is connected, test inputs) and Connect device (QR, address)
	{
		auto *connectPage = new QWidget(this);
		auto *connectLayout = new QHBoxLayout(connectPage);
		connectLayout->setContentsMargins(0, 0, 0, 0);
		connectLayout->setSpacing(16);
		connectLayout->addWidget(ui->connectionCard, 1);
		connectLayout->addWidget(ui->qrCard);
		ui->verticalLayout->removeItem(ui->contentLayout);
		delete ui->contentLayout;

		// Controllers list: one card per slot
		auto *listPage = new QWidget(this);
		auto *listLayout = new QVBoxLayout(listPage);
		listLayout->setContentsMargins(0, 0, 0, 0);
		listLayout->setSpacing(12);
		summaryLabel = new QLabel(listPage);
		summaryLabel->setStyleSheet("font-weight: 600; font-size: 12pt;");
		auto *summaryRow = new QHBoxLayout();
		summaryLabel->setTextFormat(Qt::RichText);
		summaryRow->addWidget(summaryLabel);
		summaryRow->addStretch();
		{
			const qreal dpr = devicePixelRatioF();
			auto *viewSwitch = new QFrame(listPage);
			viewSwitch->setObjectName("viewSwitch");
			viewSwitch->setStyleSheet("QFrame#viewSwitch { background: #101014; border: 1px solid #2A2A30; border-radius: 11px; }");
			auto *switchLayout = new QHBoxLayout(viewSwitch);
			switchLayout->setContentsMargins(3, 3, 3, 3);
			switchLayout->setSpacing(2);
			auto *switchGroup = new QButtonGroup(viewSwitch);
			switchGroup->setExclusive(true);
			const auto makeOption = [&](const QString &title, const char *body) {
				auto *option = new QPushButton(title, viewSwitch);
				QIcon icon;
				icon.addPixmap(svgPixmap(body, QColor("#F2F2F2"), 16, dpr), QIcon::Normal, QIcon::On);
				icon.addPixmap(svgPixmap(body, QColor("#8C8C94"), 16, dpr), QIcon::Normal, QIcon::Off);
				option->setIcon(icon);
				option->setIconSize(QSize(16, 16));
				option->setCheckable(true);
				option->setCursor(Qt::PointingHandCursor);
				option->setFixedSize(128, 32);
				option->setStyleSheet("QPushButton { background: transparent; border: 0; border-radius: 8px; color: #8C8C94; font-weight: 600; padding: 0 10px; } "
									  "QPushButton:hover:!checked { color: #CFCFD5; } "
									  "QPushButton:checked { background: #2C2C34; color: #F2F2F2; } "
									  "QPushButton:focus { outline: none; }");
				switchGroup->addButton(option);
				switchLayout->addWidget(option);
				return option;
			};
			viewStatus = makeOption(tr("Status"), kGrid);
			viewInput = makeOption(tr("Input test"), kActivity);
			viewStatus->setChecked(true);
			connect(viewStatus, &QPushButton::clicked, this, [this]() { setTestMode(false); });
			connect(viewInput, &QPushButton::clicked, this, [this]() { setTestMode(true); });
			summaryRow->addWidget(viewSwitch);
		}
		listLayout->addLayout(summaryRow);
		// Six permanent tiles, three across and two down. Empty ones are grey, a connected one takes its colour
		cardsGrid = new QGridLayout();
		cardsGrid->setSpacing(14);
		for (int column = 0; column < 4; ++column)
			cardsGrid->setColumnStretch(column, 1);
		for (int row = 0; row < 2; ++row)
			cardsGrid->setRowStretch(row, 1);
		auto *cardsLayout = cardsGrid;
		for (int i = 0; i < 8; ++i)
		{
			SlotCard card;
			card.frame = new QFrame(listPage);
			card.frame->setObjectName("slotTile");
			card.frame->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
			card.frame->installEventFilter(this);
			auto *cardLayout = new QVBoxLayout(card.frame);
			cardLayout->setContentsMargins(16, 12, 16, 14);
			cardLayout->setSpacing(6);

			card.badge = new QLabel(QString::number(i + 1), card.frame);
			// The number floats in the corner so the artwork can sit higher
			card.badge->setFixedSize(32, 28);
			card.badge->move(16, 10);

			card.deviceIcon = new QLabel(card.frame);
			card.deviceIcon->setAlignment(Qt::AlignCenter);
			card.deviceIcon->setMinimumSize(10, 80);
			card.deviceIcon->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
			card.deviceIcon->installEventFilter(this);
			cardLayout->addWidget(card.deviceIcon, 3);

			// Everything below the icon only exists while a device is connected, but keeps its space
			card.details = new QWidget(card.frame);
			auto *detailsLayout = new QVBoxLayout(card.details);
			detailsLayout->setContentsMargins(0, 0, 0, 0);
			detailsLayout->setSpacing(4);
			card.title = new QLabel(card.details);
			card.title->setAlignment(Qt::AlignHCenter);
			card.title->setMinimumWidth(0);
			card.title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
			card.detail = new QLabel(card.details);
			card.detail->setAlignment(Qt::AlignHCenter);
			card.detail->setMinimumWidth(0);
			card.detail->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
			detailsLayout->addStretch(1);
			detailsLayout->addWidget(card.title);
			detailsLayout->addWidget(card.detail);

			const auto makeChip = [&](QLabel *&text) {
				auto *chip = new QFrame(card.details);
				auto *chipLayout = new QHBoxLayout(chip);
				chipLayout->setContentsMargins(10, 2, 10, 2);
				text = new QLabel(chip);
				text->setStyleSheet("background: transparent; border: none; font-weight: 600; font-size: 9pt;");
				chipLayout->addWidget(text);
				return chip;
			};
			card.chips = new QWidget(card.details);
			auto *chips = new QHBoxLayout(card.chips);
			chips->setContentsMargins(0, 0, 0, 0);
			chips->setSpacing(6);
			chips->addStretch();
			chips->addWidget(makeChip(card.ip));
			chips->addWidget(makeChip(card.outText));
			chips->addStretch();
			detailsLayout->addWidget(card.chips);
			

			card.test = new QPushButton(tr("Test controller"), card.details);
			card.test->setCursor(Qt::PointingHandCursor);
			card.test->setMinimumHeight(34);
			connect(card.test, &QPushButton::clicked, this, [this]() {
					userPickedTab = true;
					showTab(1);
				});
			detailsLayout->addStretch(1);
			// Equal to the blank strip under the artwork, so the text sits halfway between artwork and button
			card.padSpacer = new QSpacerItem(0, 0, QSizePolicy::Minimum, QSizePolicy::Fixed);
			detailsLayout->addItem(card.padSpacer);
			detailsLayout->addWidget(card.test);

			card.disconnect = new QPushButton(tr("Disconnect"), card.details);
			card.disconnect->setCursor(Qt::PointingHandCursor);
			card.disconnect->setMinimumHeight(34);
			card.disconnect->setStyleSheet("QPushButton { color: #E5484D; border: 1px solid #5A2A2D; border-radius: 4px; background: transparent; } "
										   "QPushButton:hover { background: #2A1517; border-color: #E5484D; } "
										   "QPushButton:pressed { background: #1C0E10; }");
			card.disconnect->hide();
			const int slotNumber = i + 1;
			connect(card.disconnect, &QPushButton::clicked, this, [this, slotNumber]() { disconnectSlot(slotNumber); });
			detailsLayout->addWidget(card.disconnect);

			QSizePolicy keep = card.details->sizePolicy();
			keep.setRetainSizeWhenHidden(true);
			card.details->setSizePolicy(keep);
			cardLayout->addWidget(card.details, 2);
			// Live view that replaces the artwork and text while testing
			card.live = new ControllerTester(card.frame);
			card.live->setCompact(true);
			card.live->setSlot(i + 1);
			card.live->hide();
			cardLayout->addWidget(card.live, 1);
			cardsGrid->addWidget(card.frame, i / 4, i % 4);
			slotCards.push_back(card);
		}
		listLayout->addLayout(cardsLayout, 1);

		// Pointer to the pairing screen once at least one phone is connected
		addPhoneCard = new QFrame(listPage);
		auto *addLayout = new QHBoxLayout(addPhoneCard);
		addLayout->setContentsMargins(0, 0, 0, 0);
		addLayout->setSpacing(10);
		auto *addButton = new QPushButton(tr("Add another controller"), addPhoneCard);
		addButton->setCursor(Qt::PointingHandCursor);
		addButton->setMinimumHeight(34);
		connect(addButton, &QPushButton::clicked, this, [this]() { userPickedTab = true; showTab(1); });
		addLayout->addWidget(addButton);
		addLayout->addStretch();
		listLayout->addWidget(addPhoneCard);
		addPhoneCard->hide();

		slotNote = new QLabel(listPage);
		slotNote->setProperty("role", "caption");
		slotNote->setWordWrap(true);
		listLayout->addWidget(slotNote);

		pages = new QStackedWidget(this);
		// Wrapped labels report a very tall minimum at narrow widths, which forced the whole page to scroll
		pages->addWidget(listPage);
		pages->addWidget(connectPage);

		// Tab bar
		auto *tabRow = new QHBoxLayout();
		tabRow->setSpacing(6);
		const qreal dpr = devicePixelRatioF();
		tabPix[0] = [dpr](const QColor &c) { return glyphPixmap(Glyph::Gamepad, c, 18, dpr); };
		tabPix[1] = [dpr](const QColor &c) { return svgPixmap(kLink, c, 18, dpr); };
		auto *tabGroup = new QButtonGroup(this);
		tabGroup->setExclusive(true);
		const QString titles[2] = {tr("Controllers"), tr("Connect device")};
		QPushButton *tabs[2];
		for (int i = 0; i < 2; ++i)
		{
			auto *tab = new QPushButton(this);
			tab->setCheckable(true);
			tab->setCursor(Qt::PointingHandCursor);
			tab->setFixedHeight(68);
			tab->setStyleSheet("QPushButton { background: transparent; border: 0; border-bottom: 2px solid transparent; border-radius: 0; padding: 0; } "
							   "QPushButton:checked { border-bottom: 2px solid #F2F2F2; } "
							   "QPushButton:focus { outline: none; }");
			auto *layout = new QHBoxLayout(tab);
			layout->setContentsMargins(4, 0, 4, 0);
			layout->setSpacing(8);
			tabIcon[i] = new QLabel(tab);
			tabIcon[i]->setFixedSize(18, 18);
			tabText[i] = new QLabel(titles[i], tab);
			layout->addWidget(tabIcon[i]);
			layout->addWidget(tabText[i]);
			if (i == 0)
			{
				tabCount = new QLabel("0", tab);
				tabCount->setFixedSize(26, 20);
				tabCount->setAlignment(Qt::AlignCenter);
				layout->addWidget(tabCount);
			}
			for (QLabel *label : tab->findChildren<QLabel *>())
				label->setAttribute(Qt::WA_TransparentForMouseEvents);
			tab->setFixedWidth(layout->sizeHint().width() + 8);
			tabGroup->addButton(tab);
			tabRow->addWidget(tab);
			tabs[i] = tab;
		}
		tabControllers = tabs[0];
		tabConnect = tabs[1];
		tabRow->setSpacing(22);
		tabRow->addStretch();
		connect(tabControllers, &QPushButton::clicked, this, [this]() { userPickedTab = true; showTab(0); });
		connect(tabConnect, &QPushButton::clicked, this, [this]() { userPickedTab = true; showTab(1); });
		ui->verticalLayout->insertLayout(1, tabRow);
		// A holder without a layout never reports height-for-width, which is what made the scroll area
		// stretch the page to the tallest wrapped text layout
		pagesHolder = new QWidget(this);
		pagesHolder->setMinimumSize(560, 480);
		pagesHolder->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
		pages->setParent(pagesHolder);
		pagesHolder->installEventFilter(this);
		ui->verticalLayout->insertWidget(2, pagesHolder, 1);
		ui->clientLabel->clear();
		ui->clientLabel->hide();
		showTab(1);
		updateSlots();
	}
	clientConnection = nullptr;
	tcpServer = new QTcpServer(this);
	isGamepadConnected = false;

	// Initialize the executor with try-catch for better error handling
	switch (ExecutorType executorType = SettingsSingleton::instance().executorType())
	{
	case ExecutorType::VigemExecutor:
	case ExecutorType::GamepadExecutor:
		// A virtual gamepad is created for each phone when it connects
		qInfo() << "GamepadExecutor will be created per connection";
		break;
	case ExecutorType::KeyboardMouseExecutor:
		executor = std::make_unique<KeyboardMouseExecutor>();
		qInfo() << "KeyboardMouseExecutor initialized successfully";
		break;
	default:
		qCritical() << "Unknown executor type";
	}

	initServer();

	// The home screen connects to statusChanged after construction, so announce the state again
	QTimer::singleShot(0, this, [this]() { updateStatus(); });
	flattenLabels();
	qDebug() << "Server widget initialized";
}

Server::~Server()
{
	// IMPORTANT: clientConnection should not be accessed when the server is closed
	for (auto &client : clients)
	{
		// Closing a socket fires its disconnected handler, which erases from this list while it is walked
		client->socket->disconnect(this);
		if (client->executor)
			client->executor->release_all();
		else if (client->pad != nullptr)
			client->pad->release_all();
	}
	if (executor)
		executor->release_all();
	for (auto &client : clients)
		client->socket->close(); // Close connections gracefully
	clients.clear();
	clientConnection = nullptr;
	isGamepadConnected = false;
	tcpServer->close(); // And then close the server
	qInfo() << "Server stopped.";
	tcpServer->deleteLater();
	delete ui;
}

void Server::initServer()
{
	qInfo() << "Starting TCP server initialization";

	tcpServer->setListenBacklogSize(0);
	int port = SettingsSingleton::instance().port();
	if (port < 1024 || port > 65535)
	{
		port = 0; // 0 means random port
	}

	// A fixed port that is already taken falls back to a free one instead of failing
	if (!tcpServer->listen(QHostAddress::AnyIPv4, port) && !(port != 0 && tcpServer->listen(QHostAddress::AnyIPv4, 0)))
	{
		QMessageBox::critical(this, tr("AirDeck Server"),
							  tr("Unable to start the server: %1.").arg(tcpServer->errorString()));
		close(); // Close the error dialog
		tcpServer->close();
		setServerState(2);
		return;
	}
	// Nobody connects until stray pads are gone, or the new pad would be removed with them
	debugLog(QStringLiteral("Listening on port %1, input method %2").arg(tcpServer->serverPort()).arg(static_cast<int>(serverMode)));
	{
		auto *traffic = new QTimer(this);
		connect(traffic, &QTimer::timeout, this, [this]() {
			if (!DebugLog::enabled())
				return;
			for (auto &c : clients)
			{
				if (c->states == 0)
					continue;
				debugLog(QStringLiteral("Traffic from slot %1 \"%2\": %3 states in 5 s").arg(c->slot).arg(c->name).arg(c->states));
				c->states = 0;
			}
		});
		traffic->start(5000);

		auto *pings = new QTimer(this);
		connect(pings, &QTimer::timeout, this, &Server::sendPings);
		pings->start(2000);

		auto *status = new QTimer(this);
		connect(status, &QTimer::timeout, this, [this]() {
			if (!DebugLog::enabled())
				return;
			static const qint64 startedMs = QDateTime::currentMSecsSinceEpoch();
			qint64 workingSetMb = -1;
#ifdef _WIN32
			PROCESS_MEMORY_COUNTERS counters{};
			counters.cb = sizeof(counters);
			if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
				workingSetMb = static_cast<qint64>(counters.WorkingSetSize / (1024 * 1024));
#endif
			int active = 0;
			qint64 buffered = 0;
			for (const auto &c : clients)
			{
				active += c->active ? 1 : 0;
				buffered += c->buffer.size();
			}
			debugLog(QStringLiteral("Status: %1 connection(s), %2 active, %3 Xbox pad(s), %4 s up, memory %5 MB, %6 bytes buffered, log %7 KB")
						 .arg(clients.size()).arg(active).arg(xboxPads.size())
						 .arg((QDateTime::currentMSecsSinceEpoch() - startedMs) / 1000)
						 .arg(workingSetMb).arg(buffered).arg(QFileInfo(DebugLog::path()).size() / 1024));
		});
		status->start(30000);
	}
	tcpServer->pauseAccepting();
	releaseAllPads([this]() { tcpServer->resumeAccepting(); });
	ui->IPList->setFixedHeight(120);
	ui->QRViewer->setFixedHeight(290);
	// Same container size for the App and Browser codes
	ui->QRViewer->setFixedWidth(344);
	ui->qrCard->setFixedWidth(380);
	{
		const int twoLines = ui->hintLabel->fontMetrics().lineSpacing() * 2 + 6;
		ui->hintLabel->setMinimumHeight(twoLines);
		ui->hintLabel->setAlignment(Qt::AlignTop | Qt::AlignLeft);
		ui->qrCaption->setMinimumHeight(twoLines);
		ui->qrCaption->setWordWrap(true);
		ui->qrCaption->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
		ui->connectionLayout->insertStretch(ui->connectionLayout->indexOf(ui->copyButton), 1);
	}
	ui->statusLabel->setText(tr("Server running"));
	ui->addressTitle->setText(tr("IP address"));
	ui->portLabel->setText(tr("Port: **%1**").arg(tcpServer->serverPort()));
	ui->portLabel->setTextFormat(Qt::MarkdownText);

	// Most phones join the same Wi-Fi as this PC, which is almost always a private range.
	// Show those first and mark the most likely one so virtual adapters are not mistaken for it.
	auto rank = [](const QHostAddress &address) {
		const QString text = address.toString();
		if (text.startsWith("192.168."))
			return 0;
		if (text.startsWith("10."))
			return 1;
		if (text.startsWith("172."))
			return 2;
		return 3;
	};
	QList<QHostAddress> candidates;
	for (const QHostAddress &entry : QNetworkInterface::allAddresses())
	{
		if (entry.isGlobal() && entry.protocol() == QAbstractSocket::IPv4Protocol)
			candidates.append(entry);
	}
	std::stable_sort(candidates.begin(), candidates.end(),
					 [&rank](const QHostAddress &a, const QHostAddress &b) {
						 return rank(a) < rank(b);
					 });
	for (int i = 0; i < candidates.size(); ++i)
	{
		const QHostAddress &entry = candidates.at(i);
		const QString address = entry.toString();
		auto *item = new QListWidgetItem(i == 0 ? tr("%1   (recommended)").arg(address)
												: tr("%1   (other adapter)").arg(address));
		item->setData(Qt::UserRole, address);
		ui->IPList->addItem(item);
		QLabel *QRWidget = new QLabel();
		QRWidget->setAlignment(Qt::AlignCenter);
		ui->QRViewer->addWidget(QRWidget);
		qrLabels.push_back(QRWidget);
		qrAddresses << address;
	}
	if (ui->IPList->count() > 0)
	{
		ui->IPList->setCurrentRow(0);
	}
	else
	{
		ui->hintLabel->setText(
			tr("No network address found. Connect this PC to a network and restart the server."));
		ui->copyButton->setEnabled(false);
	}
	// Two QR codes per address: one for the phone app, one that opens the browser controller
	{
		auto *toggleRow = new QHBoxLayout();
		toggleRow->setSpacing(6);
		auto *appButton = new QPushButton(tr("App"), this);
		auto *webButton = new QPushButton(tr("Browser"), this);
		for (QPushButton *button : {appButton, webButton})
		{
			button->setCheckable(true);
			button->setCursor(Qt::PointingHandCursor);
			button->setFixedHeight(30);
			button->setStyleSheet("QPushButton { padding: 2px 14px; min-height: 18px; } "
				"QPushButton:checked { background-color: #F5F5F5; color: #000000; border-color: #FFFFFF; }");
		}
		appButton->setChecked(true);
		toggleRow->addStretch();
		toggleRow->addWidget(appButton);
		toggleRow->addWidget(webButton);
		toggleRow->addStretch();
		ui->qrLayout->insertLayout(0, toggleRow);
		const QString appHint = ui->hintLabel->text();
		const auto choose = [this, appButton, webButton, appHint](bool web) {
			webQr = web;
			appButton->setChecked(!web);
			webButton->setChecked(web);
			updateQrPixmaps();
			if (webUrlLabel != nullptr)
			{
				if (QListWidgetItem *current = ui->IPList->currentItem())
					webUrlLabel->setText(tr("http://%1:%2/")
											 .arg(current->data(Qt::UserRole).toString())
											 .arg(tcpServer->serverPort()));
				webUrlLabel->setVisible(web);
			}
			if (ui->IPList->count() > 0)
				ui->hintLabel->setText(web ? tr("Open the address below in the phone's browser, or scan the QR code with its camera.")
										   : appHint);
		flattenLabels();
			};
		connect(appButton, &QPushButton::clicked, this, [choose]() { choose(false); });
		connect(webButton, &QPushButton::clicked, this, [choose]() { choose(true); });

		webUrlLabel = new QLabel(this);
		webUrlLabel->setProperty("role", "caption");
		webUrlLabel->setWordWrap(true);
		webUrlLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
		{
			// The web address row keeps its space in App mode so nothing below it shifts
			QSizePolicy keep = webUrlLabel->sizePolicy();
			keep.setRetainSizeWhenHidden(true);
			webUrlLabel->setSizePolicy(keep);
			webUrlLabel->setMinimumHeight(webUrlLabel->fontMetrics().lineSpacing() + 4);
		}
		webUrlLabel->setVisible(false);
		ui->connectionLayout->insertWidget(ui->connectionLayout->indexOf(ui->portLabel) + 1, webUrlLabel);
		updateQrPixmaps();
	}
	connect(ui->copyButton, &QPushButton::clicked, this, [this]() {
		QListWidgetItem *current = ui->IPList->currentItem();
		if (current == nullptr)
			return;
		const QString address = current->data(Qt::UserRole).toString();
		QApplication::clipboard()->setText(webQr ? tr("http://%1:%2/").arg(address).arg(tcpServer->serverPort())
												 : tr("%1:%2").arg(address).arg(tcpServer->serverPort()));
		ui->copyButton->setText(tr("Copied"));
		QTimer::singleShot(1500, ui->copyButton, [this]() { ui->copyButton->setText(tr("Copy address")); });
	});
	connect(ui->IPList, &QListWidget::currentItemChanged, this,
			[this](QListWidgetItem *current, QListWidgetItem *) {
				if (current != nullptr)
				{
						ui->QRViewer->setCurrentIndex(ui->IPList->row(current));
						if (webUrlLabel != nullptr)
							webUrlLabel->setText(tr("http://%1:%2/")
													 .arg(current->data(Qt::UserRole).toString())
													 .arg(tcpServer->serverPort()));
				}
			});
	connect(tcpServer, &QTcpServer::newConnection, this, &Server::handleConnection);
	auto *securityNote = new QLabel(
		tr("Anyone on your network who has this address can connect. The QR code is not a password."), this);
	securityNote->setProperty("role", "caption");
	securityNote->setWordWrap(true);
	securityNote->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
	securityNote->setMinimumHeight(securityNote->fontMetrics().lineSpacing() * 2 + 6);
	ui->qrLayout->addWidget(securityNote);
	ui->qrLayout->addStretch();
	updateStatus();
	qInfo() << "Server started successfully on port:" << tcpServer->serverPort();
}

int Server::pickSlot(const ExecutorInterface *executor) const
{
	const auto used = [this](int slot) {
		return std::any_of(clients.begin(), clients.end(),
						   [slot](const std::unique_ptr<Client> &c) { return c->slot == slot; });
	};
	const int preferred = executor != nullptr ? executor->slot() : -1;
	if (preferred >= 1 && preferred <= 8 && !used(preferred))
		return preferred;
	for (int slot = 1; slot <= 8; ++slot)
	{
		if (!used(slot))
			return slot;
	}
	return 0;
}

int Server::slotCapacity() const
{
	return serverMode == ExecutorType::KeyboardMouseExecutor ? 1 : static_cast<int>(MAX_PHONES);
}

void Server::updateStatus()
{
	const int active = static_cast<int>(std::count_if(
		clients.begin(), clients.end(), [](const std::unique_ptr<Client> &c) { return c->active; }));
	const int capacity = slotCapacity();
	const int port = tcpServer != nullptr ? tcpServer->serverPort() : 0;
	if (countPill != nullptr)
	{
		const QString modeName = serverMode == ExecutorType::VigemExecutor ? (padStyleIsPlayStation() ? tr("DualShock 4 (ViGEm)") : tr("Xbox 360 (ViGEm)"))
								 : serverMode == ExecutorType::GamepadExecutor ? tr("Windows virtual gamepad")
																			   : tr("Keyboard and mouse");
		countPill->setText(tr("%1  \u00B7  Port %2").arg(modeName).arg(port));
	}
	if (summaryLabel != nullptr)
		summaryLabel->setText(tr("%1 connected  \u00B7  %2 available").arg(active).arg(std::max(0, capacity - active)));
	if (tabCount != nullptr)
		tabCount->setText(QString::number(active));
	if (!userPickedTab && pages != nullptr)
		showTab(active > 0 ? 0 : 1);
	if (stopped)
	{
		emit statusChanged(tr("Server stopped"));
		return;
	}
	emit statusChanged(tr("Server running  |  Port %1  |  %2")
						   .arg(port)
						   .arg(active == 0 ? tr("no device connected")
											: tr("%1 of %2 connected").arg(active).arg(capacity)));
}

void Server::showTab(int index)
{
	pages->setCurrentIndex(index);
	tabControllers->setChecked(index == 0);
	tabConnect->setChecked(index == 1);
	styleTabs();
}

bool Server::debugEnabled() const
{
	return DebugLog::enabled();
}

bool Server::padStyleIsPlayStation() const
{
	return serverMode == ExecutorType::VigemExecutor &&
		   SettingsSingleton::instance().qsettings()->value("server/pad_style", "xbox").toString() == QLatin1String("playstation");
}

bool Server::multiControllerEnabled() const
{
	return SettingsSingleton::instance().qsettings()->value("server/multi_controller", false).toBool();
}

QString Server::webPageVersion()
{
	static const QString version = [] {
		QFile page(":/web/controller.html");
		if (!page.open(QIODevice::ReadOnly))
			return QString();
		return QString::fromLatin1(QCryptographicHash::hash(page.readAll(), QCryptographicHash::Md5).toHex().left(12));
	}();
	return version;
}

QString Server::debugLogPath() const
{
	return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath("debug.log");
}

void Server::debugLog(const QString &line, bool force)
{
	if (!DebugLog::enabled() && !force)
		return;
	qInfo().noquote() << "[debug]" << line;
	DebugLog::write(line, force);
}

QString Server::deviceKey(const Client &client) const
{
	bool ok = false;
	const QHostAddress address = client.socket->peerAddress();
	const quint32 v4 = address.toIPv4Address(&ok);
	return ok ? QHostAddress(v4).toString() : address.toString();
}

void Server::setTestMode(bool on)
{
	testMode = on;
	if (viewStatus != nullptr)
		viewStatus->setChecked(!on);
	if (viewInput != nullptr)
		viewInput->setChecked(on);
	updateSlots();
}

void Server::setServerState(int state)
{
	if (serverIcon == nullptr)
		return;
	const QColor color = state == 0 ? QColor("#34C759") : state == 1 ? QColor("#F5C518") : QColor("#E5484D");
	const QString tint = state == 0 ? "rgba(52, 199, 89, 0.10)" : state == 1 ? "rgba(245, 197, 24, 0.10)" : "rgba(229, 72, 77, 0.10)";
	serverIcon->setStyleSheet(QStringLiteral("background: %1; border: 1px solid #2C2C33; border-radius: 12px;").arg(tint));
	serverIcon->setPixmap(svgPixmap(kServer, color, 24, devicePixelRatioF()));
	serverIcon->setToolTip(state == 0 ? tr("Server running") : state == 1 ? tr("Server busy") : tr("Server not running"));
}

void Server::styleTabs()
{
	for (int i = 0; i < 2; ++i)
	{
		QPushButton *tab = i == 0 ? tabControllers : tabConnect;
		if (tab == nullptr || tabIcon[i] == nullptr)
			continue;
		const QColor color(tab->isChecked() ? "#F2F2F2" : "#8C8C94");
		tabIcon[i]->setPixmap(tabPix[i](color));
		tabText[i]->setStyleSheet(QStringLiteral("color: %1; font-weight: 600; background: transparent;").arg(color.name()));
	}
	if (tabCount != nullptr)
		tabCount->setStyleSheet(QStringLiteral("background: #26262C; color: %1; border-radius: 6px; font-weight: 600; font-size: 9pt;")
									.arg(tabControllers != nullptr && tabControllers->isChecked() ? "#E4E4EA" : "#8C8C94"));
}

bool Server::listenServer()
{
	int port = SettingsSingleton::instance().port();
	if (port < 1024 || port > 65535)
		port = 0;
	return tcpServer->listen(QHostAddress::AnyIPv4, port) || (port != 0 && tcpServer->listen(QHostAddress::AnyIPv4, 0));
}

void Server::dropClients()
{
	// Release every virtual controller first so no input stays held
	for (auto &client : clients)
	{
		client->socket->disconnect(this);
		if (client->executor)
			client->executor->release_all();
		else if (client->pad != nullptr)
			client->pad->release_all();
	}
	if (executor)
		executor->release_all();
	for (auto &client : clients)
	{
		client->socket->close();
		client->socket->deleteLater();
	}
	clients.clear();
	clientConnection = nullptr;
	isGamepadConnected = false;
	// Unplug the virtual Xbox pads too, so a stop or restart starts from nothing
	xboxPads.clear();
	tcpServer->close();
	updateClientLabel();
	updateSlots();
}

void Server::stopServer()
{
	if (restarting || stopped)
		return;
	dropClients();
	stopped = true;
	ui->statusLabel->setText(tr("Server stopped"));
	setServerState(2);
	ui->stopButton->setEnabled(false);
	restartBtn->setToolTip(tr("Start server"));
	restartBtn->setAccessibleName(tr("Start server"));
	debugLog(QStringLiteral("Server stopped"));
	updateStatus();
}

void Server::restartServer()
{
	if (restarting)
		return;
	const int active = static_cast<int>(std::count_if(
		clients.begin(), clients.end(), [](const std::unique_ptr<Client> &c) { return c->active; }));
	if (active > 0 && !stopped)
	{
		QMessageBox confirm(this);
		confirm.setWindowTitle(tr("Restart server"));
		confirm.setIcon(QMessageBox::Question);
		confirm.setText(tr("Restart the server?"));
		confirm.setInformativeText(tr("%n connected controller(s) will be disconnected for a moment. Browser controllers reconnect on their own.", "", active));
		auto *go = confirm.addButton(tr("Restart"), QMessageBox::AcceptRole);
		confirm.addButton(tr("Cancel"), QMessageBox::RejectRole);
		confirm.exec();
		if (confirm.clickedButton() != go)
			return;
	}
	restartNow();
}

void Server::pushRumble(int slot, unsigned char large, unsigned char weak)
{
	qInfo() << "Game rumble for controller" << slot << "large" << large << "weak" << weak;
	for (auto &client : clients)
	{
		if (!client->active || client->slot != slot)
			continue;
		if (client->protocol == Client::Protocol::Native)
		{
			client->socket->write("VGPRUMBLE:" + QByteArray::number(large) + "," + QByteArray::number(weak) + "\n");
		}
		else if (client->protocol == Client::Protocol::WebSocket)
		{
			QJsonObject message;
			message["t"] = "rumble";
			message["l"] = large;
			message["s"] = weak;
			sendWebJson(*client, message);
		}
		return;
	}
}

// Unplugging pads can take several seconds and Windows asks for permission, so it runs off the interface thread
// and the caller carries on from `done` once the pads are gone.
void Server::releaseAllPads(std::function<void()> done)
{
	if (serverMode != ExecutorType::VigemExecutor)
	{
		done();
		return;
	}
	QPointer<Server> self(this);
	auto attempts = std::make_shared<int>(0);
	auto check = std::make_shared<std::function<void()>>();
	*check = [self, attempts, check, done]() {
		// Clearing *check below destroys this closure, so everything used afterwards is copied out first
		const QPointer<Server> me = self;
		const auto tries = attempts;
		const auto keep = check;
		const std::function<void()> finish = done;
		if (!me)
			return;
		// Pads this server just dropped take a moment to disappear
		if (!findVirtualPads().isEmpty() && ++*tries <= 6)
		{
			QTimer::singleShot(150, me, *keep);
			return;
		}
		const QStringList left = findVirtualPads();
		*keep = nullptr;
		if (left.isEmpty())
		{
			finish();
			return;
		}
		qInfo() << "Unplugging" << left.size() << "virtual pad entries still on this PC";
		const bool elevated = isElevated();
		std::thread([me, finish, elevated]() {
			if (elevated)
				removeVirtualPads();
			else
				removeVirtualPadsElevated();
			QMetaObject::invokeMethod(qApp, [me, finish]() {
				if (me)
					finish();
			}, Qt::QueuedConnection);
		}).detach();
	};
	(*check)();
}

void Server::applySettings()
{
	if (stopped || restarting)
		return;
	restartNow();
}

void Server::disconnectSlot(int slot)
{
	const auto it = std::find_if(clients.begin(), clients.end(),
								 [slot](const std::unique_ptr<Client> &c) { return c->active && c->slot == slot; });
	if (it == clients.end())
		return;
	QTcpSocket *socket = (*it)->socket;
	debugLog(QStringLiteral("Disconnected controller %1 from the PC").arg(slot));
	// Browser controllers get a notice and wait a few seconds before they try again
	refuse(**it, tr("Disconnected from the PC"));
	QPointer<QTcpSocket> guard(socket);
	QTimer::singleShot(250, this, [guard]() {
		if (guard)
			guard->abort();
	});
}

void Server::restartNow()
{
	restarting = true;
	const auto lock = [this](bool enabled) {
		homeBtn->setEnabled(enabled);
		restartBtn->setEnabled(enabled);
		ui->stopButton->setEnabled(enabled);
	};
	lock(false);
	ui->statusLabel->setText(stopped ? tr("Starting server") : tr("Restarting server"));
	setServerState(1);
	debugLog(QStringLiteral("Restarting server"));

	dropClients();
	// Pick up a changed input method
	serverMode = SettingsSingleton::instance().executorType();
	executor.reset();
	if (serverMode == ExecutorType::KeyboardMouseExecutor)
		executor = std::make_unique<KeyboardMouseExecutor>();
	updateStatus();

	QTimer::singleShot(700, this, [this, lock]() {
		releaseAllPads([this, lock]() {
		if (!listenServer())
		{
			ui->statusLabel->setText(tr("Restart failed"));
			setServerState(2);
			QMessageBox::warning(this, tr("Restart server"), tr("The server could not start again: %1.").arg(tcpServer->errorString()));
			restarting = false;
			lock(true);
			if (stopped)
				ui->stopButton->setEnabled(false);
			return;
		}
		ui->portLabel->setText(tr("Port: **%1**").arg(tcpServer->serverPort()));
		if (webUrlLabel != nullptr)
		{
			if (QListWidgetItem *current = ui->IPList->currentItem())
				webUrlLabel->setText(tr("http://%1:%2/").arg(current->data(Qt::UserRole).toString()).arg(tcpServer->serverPort()));
		}
		updateQrPixmaps();
		ui->statusLabel->setText(tr("Server running"));
		setServerState(0);
		stopped = false;
		restartBtn->setToolTip(tr("Restart server"));
		restartBtn->setAccessibleName(tr("Restart server"));
		restarting = false;
		lock(true);
		updateStatus();
		debugLog(QStringLiteral("Server restarted on port %1").arg(tcpServer->serverPort()));
		});
	});
}

void Server::noteInput(Client &client, const vgp_data_exchange_gamepad_reading &reading)
{
	const quint32 before = client.held;
	client.held = (client.held | reading.buttons_down) & ~reading.buttons_up;
	if (client.held != before)
		debugLog(QStringLiteral("Slot %1 buttons 0x%2 -> 0x%3").arg(client.slot).arg(before, 0, 16).arg(client.held, 0, 16));
	client.lx = reading.left_thumbstick_x;
	client.ly = reading.left_thumbstick_y;
	client.rx = reading.right_thumbstick_x;
	client.ry = reading.right_thumbstick_y;
	client.lt = reading.left_trigger;
	client.rt = reading.right_trigger;
	if (testMode && client.slot >= 1 && client.slot <= static_cast<int>(slotCards.size()))
		slotCards[client.slot - 1].live->setState(client.held, client.lx, client.ly, client.rx, client.ry, client.lt,
												   client.rt);
}

QString Server::deviceTitle(const Client &client) const
{
	const bool web = client.protocol == Client::Protocol::WebSocket;
	if (!web)
		return !client.name.isEmpty() ? client.name : tr("Phone app");
	const QString n = client.name;
	const bool generic = n.isEmpty() || n == QStringLiteral("Browser") || n.endsWith(QStringLiteral(" device")) ||
						 n == QStringLiteral("iPhone") || n == QStringLiteral("iPad");
	return generic ? tr("Browser controller") : n;
}

QString Server::outputName(const Client &client) const
{
	if (client.ds4)
		return tr("DualShock 4");
	switch (serverMode)
	{
	case ExecutorType::VigemExecutor:
		return tr("Xbox 360");
	case ExecutorType::GamepadExecutor:
		return tr("Windows gamepad");
	default:
		return tr("Keyboard / Mouse");
	}
}

void Server::refreshIcon(SlotCard &card)
{
	const QSize size = card.deviceIcon->size();
	if (size.width() <= 8 || size.height() <= 8 || !card.iconColor.isValid())
		return;
	const qreal dpr = devicePixelRatioF();
	// Empty and connected slots draw the controller at the same size, in the middle of the same space
	QPixmap full(size * dpr);
	full.setDevicePixelRatio(dpr);
	full.fill(Qt::transparent);
	const QSize inner(size.width() * 0.55, size.height() * 0.62);
	if (card.padSpacer != nullptr)
	{
		const double fit = std::min((inner.width() - 8) / 200.0, (inner.height() - 8) / 148.0);
		const int pad = std::max(0, static_cast<int>((size.height() - 148 * fit) / 2));
		if (card.padSpacer->sizeHint().height() != pad)
		{
			card.padSpacer->changeSize(0, pad, QSizePolicy::Minimum, QSizePolicy::Fixed);
			card.details->layout()->invalidate();
		}
	}
	QPainter p(&full);
	p.drawPixmap((size.width() - inner.width()) / 2, (size.height() - inner.height()) / 2,
				 controllerPixmap(card.iconPs4, card.iconColor, inner, dpr));
	p.end();
	card.deviceIcon->setPixmap(full);
}

void Server::updateSlots()
{
	std::vector<int> freeSlots;
	for (int i = 0; i < static_cast<int>(slotCards.size()); ++i)
	{
		const Client *owner = nullptr;
		for (const auto &client : clients)
		{
			if (client->active && client->slot == i + 1)
				owner = client.get();
		}
		SlotCard &card = slotCards[i];
		const bool active = owner != nullptr;
		const bool usable = i < slotCapacity();
		card.occupied = active;
		if (!active && usable)
			freeSlots.push_back(i + 1);
		// Controllers 5 to 8 are always the PlayStation style ones, and so is every slot when PlayStation is chosen
		const bool psStyle = padStyleIsPlayStation();
		const bool ps4 = i >= 4 || psStyle;
		const QColor slot = slotColor(i + 1);
		const QColor art = active ? slot : QColor("#4A4A52");
		const QString slotRgb = QString("%1,%2,%3").arg(slot.red()).arg(slot.green()).arg(slot.blue());

		// Slot colour only on the artwork, the number and a quiet border; the rest stays neutral
		card.frame->setStyleSheet(QString("QFrame#slotTile { background: #0C0C0E; border: 1px solid %1; border-radius: 6px; } "
										  "QFrame#slotTile QLabel { background: transparent; border: none; }")
									  .arg(active ? QString("rgba(%1,140)").arg(slotRgb) : QStringLiteral("#2A2A30")));
		card.frame->setCursor(active ? Qt::ArrowCursor : Qt::PointingHandCursor);
		QString tip = active ? QString() : (usable ? tr("Available. Click to connect a device.") : tr("Not available in this mode"));
		if (ps4)
		{
			const QString note = psStyle ? tr("Appears as a PlayStation controller. Games with PlayStation support show PlayStation buttons.")
										 : tr("Appears as a PlayStation controller. Games that only read XInput will not see it.");
			tip = tip.isEmpty() ? note : tip + QStringLiteral("\n") + note;
		}
		card.frame->setToolTip(tip);
		// Same number style for every slot, grey until a controller is connected
		card.badge->setText(QString::number(i + 1));
		card.badge->setFixedSize(32, 28);
		card.badge->move(16, 10);
		card.badge->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
		card.badge->setStyleSheet(QString("font-weight: 800; font-size: 15pt; color: %1;")
									  .arg(active ? slot.name() : QStringLiteral("#4A4A52")));
		card.iconPs4 = ps4;
		card.iconColor = art;
		refreshIcon(card);

		// Testing turns a connected tile into a live controller
		const bool live = testMode && active;
		card.live->setVisible(live);
		card.deviceIcon->setVisible(!live);
		card.details->setVisible(!live);
		{
			// While testing, the live controller takes all the room the text block would keep
			QSizePolicy detailsPolicy = card.details->sizePolicy();
			detailsPolicy.setRetainSizeWhenHidden(!live);
			card.details->setSizePolicy(detailsPolicy);
		}
		if (live)
		{
			card.live->setSlot(i + 1);
			card.live->setPs4(ps4);
			card.live->setState(owner->held, owner->lx, owner->ly, owner->rx, owner->ry, owner->lt, owner->rt);
		}

		// Only the status text remains in an empty tile, so it can sit midway between the artwork and the button
		card.chips->setVisible(active);
		card.detail->setVisible(active);
		card.test->setText(tr("+  Connect device"));
		card.test->setVisible(!active);
		card.disconnect->setVisible(active);
		card.test->setStyleSheet("QPushButton { color: #F5F5F5; border: 1px solid #4A4A52; border-radius: 4px; background: transparent; } "
								 "QPushButton:hover { background: #1E1E22; border-color: #6A6A72; } "
								 "QPushButton:pressed { background: #141416; }");
		if (!active)
		{
			card.title->setText(tr("Waiting for controller"));
			card.title->setStyleSheet("font-size: 11pt; font-weight: 600; color: #8A8A92;");
			card.detail->setText(QStringLiteral("\u00A0"));
			continue;
		}

		const bool web = owner->protocol == Client::Protocol::WebSocket;
		card.title->setText(deviceTitle(*owner));
		card.title->setStyleSheet("font-weight: 700; font-size: 12pt; color: #F5F5F5;");
		// Platform and browser for web controllers, the phone model for the app
		QString detail = owner->model;
		const int sizeAt = detail.lastIndexOf(QStringLiteral(" ("));
		if (web && sizeAt > 0 && detail.endsWith(QLatin1Char(')')))
			detail.truncate(sizeAt);
		if (web)
		{
			detail.replace(QStringLiteral(", "), QStringLiteral("  \u00B7  "));
			if (detail.isEmpty())
				detail = tr("Web browser");
		}
		else
		{
			detail = (!owner->model.isEmpty() && owner->model != owner->name) ? tr("Android app  \u00B7  %1").arg(owner->model)
																			   : tr("Android app");
		}
		card.detail->setText(detail);
		card.detail->setToolTip(owner->model);
		card.detail->setStyleSheet("font-size: 9pt; color: #9A9AA2;");
		const QString chipStyle = "QFrame { background: #1F1F24; border: 1px solid #34343A; border-radius: 10px; } "
								  "QLabel { color: #D8D8DE; }";
		card.ip->setText(owner->socket->peerAddress().toString());
		card.ip->parentWidget()->setStyleSheet(chipStyle);
		card.outText->setText(outputName(*owner));
		card.outText->parentWidget()->setStyleSheet(chipStyle);
	}
	if (slotNote != nullptr)
		slotNote->hide();
}

void Server::updateClientLabel()
{
	updateStatus();
	// Only refusals use this line; connected phones are shown on their cards
	ui->clientLabel->clear();
}

ExecutorInterface *Server::executorFor(Client &client)
{
	if (client.pad != nullptr)
		return client.pad;
	return client.executor ? client.executor.get() : executor.get();
}

bool Server::activate(Client &client, QString *failure)
{
	if (!multiControllerEnabled())
	{
		const QString key = deviceKey(client);
		for (const auto &other : clients)
		{
			if (other.get() != &client && other->active && deviceKey(*other) == key)
			{
				*failure = tr("This device already has a controller connected. Allow several controllers per device in Preferences to add more.");
				return false;
			}
		}
	}
	// Keyboard/mouse input is shared, so only one phone may drive it.
	// Virtual gamepads can be created per phone.
	const ExecutorType mode = SettingsSingleton::instance().executorType();
	const bool multiPhone =
		mode == ExecutorType::GamepadExecutor || mode == ExecutorType::VigemExecutor;
	const auto slotUsed = [this](int slot) {
		return std::any_of(clients.begin(), clients.end(),
						   [slot](const std::unique_ptr<Client> &c) { return c->slot == slot; });
	};
	int chosenSlot = 0;
	if (!multiPhone)
	{
		for (const auto &other : clients)
		{
			if (other.get() != &client && other->active)
			{
				*failure = tr("Another phone is already connected. Keyboard/Mouse mode accepts one phone.");
				return false;
			}
		}
	}
	else if (mode == ExecutorType::VigemExecutor)
	{
#ifdef _WIN32
		const int xboxInUse = static_cast<int>(std::count_if(
			clients.begin(), clients.end(),
			[](const std::unique_ptr<Client> &c) { return c->active && !c->ds4; }));
		const bool psStyle = padStyleIsPlayStation();
		const bool useDs4 = psStyle || xboxInUse >= 4;
		if (useDs4)
		{
			const std::vector<int> candidates = psStyle ? std::vector<int>{1, 2, 3, 4, 5, 6, 7, 8} : std::vector<int>{5, 6, 7, 8};
			for (int candidate : candidates)
			{
				if (!slotUsed(candidate))
				{
					chosenSlot = candidate;
					break;
				}
			}
			if (chosenSlot == 0)
			{
				*failure = tr("All 8 controller slots are in use.");
				return false;
			}
		}
		if (useDs4)
		{
			try
			{
				client.executor = std::make_unique<ViGEmExecutor>(true, chosenSlot, -1);
				client.executor->setRumbleCallback([this, chosenSlot](unsigned char l, unsigned char s) {
					QMetaObject::invokeMethod(this, [this, chosenSlot, l, s]() { pushRumble(chosenSlot, l, s); }, Qt::QueuedConnection);
				});
			}
			catch (const std::exception &e)
			{
				*failure = QString::fromUtf8(e.what());
				return false;
			}
		}
		else
		{
			// Xbox pads stay plugged in, one per slot in XInput order, so player N is always controller N
			int wanted = 0;
			for (int candidate = 1; candidate <= 4 && wanted == 0; ++candidate)
			{
				if (!slotUsed(candidate))
					wanted = candidate;
			}
			if (wanted == 0)
			{
				*failure = tr("All controller slots are in use.");
				return false;
			}
			try
			{
				while (static_cast<int>(xboxPads.size()) < wanted)
				{
					const int padSlot = static_cast<int>(xboxPads.size()) + 1;
					xboxPads.push_back(std::make_unique<ViGEmExecutor>(false, -1, padSlot - 1));
					xboxPads.back()->setRumbleCallback([this, padSlot](unsigned char l, unsigned char s) {
						QMetaObject::invokeMethod(this, [this, padSlot, l, s]() { pushRumble(padSlot, l, s); }, Qt::QueuedConnection);
					});
				}
			}
			catch (const std::exception &e)
			{
				*failure = QString::fromUtf8(e.what());
				return false;
			}
			client.pad = xboxPads[wanted - 1].get();
			if (client.pad->slot() != wanted)
				qWarning() << "Player" << wanted << "is XInput controller" << client.pad->slot()
						   << "because another virtual pad is still plugged in";
			// The first controller should be controller 1 for Windows too. If not, start over once: the restart
			// clears stray pads. A second mismatch in a row is left alone so this cannot loop.
			if (wanted == 1 && client.pad->slot() > 0)
			{
				if (client.pad->slot() == 1)
				{
					numberingRetried = false;
				}
				else if (!numberingRetried)
				{
					numberingRetried = true;
					debugLog(QStringLiteral("Controller 1 is XInput controller %1, restarting the server").arg(client.pad->slot()));
					QTimer::singleShot(800, this, [this]() {
						if (!restarting)
							restartNow();
					});
				}
			}
			chosenSlot = wanted;
		}
		client.ds4 = useDs4;
#endif
	}
	else
	{
		bool crashed = false;
		client.executor.reset(createGamepadGuarded(&crashed));
		if (!client.executor)
		{
			*failure = tr("Could not create a virtual gamepad. Switch to Keyboard/Mouse "
						  "Simulation or the ViGEm controller in Preferences.");
			return false;
		}
	}
	client.slot = chosenSlot != 0 ? chosenSlot : pickSlot(client.executor.get());
	if (client.slot == 0)
	{
		*failure = tr("All 8 controller slots are in use.");
		return false;
	}
	client.active = true;
	clientConnection = client.socket;
	isGamepadConnected = true;
	qInfo() << "Device connected:" << client.socket->peerAddress().toString()
			<< "slot" << client.slot << "total devices:" << clients.size();
	debugLog(QStringLiteral("Connected %1 (%2) as slot %3, devices: %4")
				 .arg(client.socket->peerAddress().toString(), client.protocol == Client::Protocol::WebSocket ? "browser" : "app")
				 .arg(client.slot)
				 .arg(clients.size()));
	updateClientLabel();
	updateSlots();
	return true;
}

static qint64 nowNs()
{
	static QElapsedTimer clock;
	if (!clock.isValid())
		clock.start();
	return clock.nsecsElapsed();
}

void Server::sendPings()
{
	for (auto &c : clients)
	{
		if (!c->active || c->pending)
			continue;
		const bool native = c->protocol == Client::Protocol::Native && c->pingSupported;
		const bool web = c->protocol == Client::Protocol::WebSocket;
		if (!native && !web)
			continue;
		c->pingId++;
		c->pingSentNs = nowNs();
		if (native)
			c->socket->write("VGPPING:" + QByteArray::number(c->pingId) + "\n");
		else
			sendWebJson(*c, QJsonObject{{"t", "ping"}, {"id", static_cast<qint64>(c->pingId)}});
	}
}

void Server::notePong(Client &client, quint32 id)
{
	if (id != client.pingId || client.pingSentNs == 0)
		return;
	const double ms = static_cast<double>(nowNs() - client.pingSentNs) / 1.0e6;
	client.pingSentNs = 0;
	client.lastRttMs = ms;
	client.rtts.push_back(ms);
	if (client.rtts.size() > 30)
		client.rtts.erase(client.rtts.begin());
	if (++client.rttSinceLog >= 15)
	{
		client.rttSinceLog = 0;
		double sum = 0, worst = 0;
		for (double v : client.rtts)
		{
			sum += v;
			worst = std::max(worst, v);
		}
		debugLog(QStringLiteral("Latency slot %1 \"%2\": %3 ms average, %4 ms worst over %5 pings").arg(client.slot).arg(client.name)
					 .arg(sum / static_cast<double>(client.rtts.size()), 0, 'f', 1).arg(worst, 0, 'f', 1).arg(client.rtts.size()));
	}
	for (SlotCard &card : slotCards)
	{
		if (card.occupied && card.ip != nullptr && client.slot > 0 && &card == &slotCards[client.slot - 1])
		{
			card.ip->setText(client.socket->peerAddress().toString() + QStringLiteral("  \u00B7  ") + latencyText(client));
			QString tip;
			double sum = 0, lo = 1e9, hi = 0;
			for (double v : client.rtts)
			{
				sum += v;
				lo = std::min(lo, v);
				hi = std::max(hi, v);
			}
			card.ip->parentWidget()->setToolTip(tr("Round trip over the last %1 pings: %2 ms best, %3 ms average, %4 ms worst")
													.arg(client.rtts.size()).arg(lo, 0, 'f', 1).arg(sum / static_cast<double>(client.rtts.size()), 0, 'f', 1).arg(hi, 0, 'f', 1));
		}
	}
}

QString Server::latencyText(const Client &client) const
{
	if (client.lastRttMs < 0)
		return QString();
	return tr("%1 ms").arg(client.lastRttMs, 0, 'f', client.lastRttMs < 10 ? 1 : 0);
}

bool Server::needsApproval(const Client &client) const
{
	auto *settings = SettingsSingleton::instance().qsettings();
	if (!settings->value("server/require_approval", false).toBool())
		return false;
	return !settings->value("server/trusted_devices").toStringList().contains(deviceKey(client));
}

void Server::askApproval(Client &client)
{
	client.pending = true;
	QPointer<QTcpSocket> socket = client.socket;
	const QString address = deviceKey(client);
	const QString kind = client.protocol == Client::Protocol::WebSocket ? tr("a web browser") : tr("the AirDeck app");
	debugLog(QStringLiteral("Waiting for approval of %1 (%2)").arg(address, kind));
	auto *box = new QMessageBox(QMessageBox::Question, tr("Allow this device?"),
								tr("A device at %1, using %2, wants to connect and control this PC.").arg(address, kind), QMessageBox::NoButton, nullptr);
	box->setAttribute(Qt::WA_DeleteOnClose);
	box->setWindowFlag(Qt::WindowStaysOnTopHint, true);
	box->setInformativeText(tr("Only allow devices you recognise. Always allow remembers this address."));
	box->addButton(tr("Allow"), QMessageBox::AcceptRole);
	box->addButton(tr("Always allow"), QMessageBox::YesRole);
	QPushButton *deny = box->addButton(tr("Deny"), QMessageBox::RejectRole);
	box->setDefaultButton(deny);
	box->setEscapeButton(deny);
	connect(box, &QDialog::finished, this, [this, socket, box](int) {
		if (socket == nullptr)
			return;
		const QAbstractButton *clicked = box->clickedButton();
		const int role = clicked != nullptr ? static_cast<int>(box->buttonRole(const_cast<QAbstractButton *>(clicked))) : static_cast<int>(QMessageBox::RejectRole);
		finishApproval(socket, role == QMessageBox::YesRole ? 2 : role == QMessageBox::AcceptRole ? 1 : 0);
	});
	if (socket != nullptr)
		connect(socket, &QAbstractSocket::disconnected, box, [box]() { box->close(); });
	box->open();
	box->raise();
	box->activateWindow();
}

void Server::finishApproval(QTcpSocket *socket, int choice)
{
	const auto it = std::find_if(clients.begin(), clients.end(), [socket](const std::unique_ptr<Client> &c) { return c->socket == socket; });
	if (it == clients.end())
		return;
	Client &client = **it;
	if (!client.pending)
		return;
	client.pending = false;
	if (choice == 0)
	{
		debugLog(QStringLiteral("Denied %1").arg(deviceKey(client)));
		refuse(client, tr("This device was not approved on the PC."));
		return;
	}
	if (choice == 2)
	{
		auto *settings = SettingsSingleton::instance().qsettings();
		QStringList trusted = settings->value("server/trusted_devices").toStringList();
		if (!trusted.contains(deviceKey(client)))
			trusted.append(deviceKey(client));
		settings->setValue("server/trusted_devices", trusted);
	}
	debugLog(QStringLiteral("Approved %1%2").arg(deviceKey(client), choice == 2 ? QStringLiteral(", remembered") : QString()));
	QString failure;
	if (!activate(client, &failure))
	{
		refuse(client, failure);
		return;
	}
	if (client.protocol == Client::Protocol::WebSocket)
	{
		sendWebJson(client, QJsonObject{{"t", "slot"}, {"n", client.slot}, {"v", webPageVersion()}});
		serveWebSocket(client);
	}
	else
	{
		client.socket->write("VGPSLOT:" + QByteArray::number(client.slot) + "\n");
		serveNative(client);
	}
}

void Server::refuse(Client &client, const QString &why)
{
	qWarning() << "Refusing a device:" << why;
	debugLog(QStringLiteral("Refused %1: %2").arg(client.socket->peerAddress().toString(), why));
	ui->clientLabel->setText(why);
	QTcpSocket *socket = client.socket;
	if (client.protocol == Client::Protocol::WebSocket)
	{
		sendWebJson(client, QJsonObject{{"t", "refused"}, {"why", why}, {"v", webPageVersion()}});
		sendWebFrame(socket, QByteArray(), 8);
	}
	socket->disconnectFromHost();
}

void Server::handleConnection()
{
	QTcpSocket *socket = tcpServer->nextPendingConnection();
	if (socket == nullptr)
		return;
	// disable Nagle's algorithm to avoid delay and bunching of small packages
	socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);

	auto client = std::make_unique<Client>();
	client->socket = socket;
	client->connectedAtMs = QDateTime::currentMSecsSinceEpoch();
	clients.push_back(std::move(client));
	debugLog(QStringLiteral("Connection from %1:%2 accepted, %3 open").arg(socket->peerAddress().toString()).arg(socket->peerPort()).arg(clients.size()));
	connect(socket, &QAbstractSocket::errorOccurred, this, [this, socket](QAbstractSocket::SocketError error) {
		debugLog(QStringLiteral("Socket error from %1: %2 (%3)").arg(socket->peerAddress().toString(), socket->errorString()).arg(static_cast<int>(error)));
	});

	connect(socket, &QAbstractSocket::disconnected, this, [this, socket]() {
		const auto it = std::find_if(clients.begin(), clients.end(),
									 [socket](const std::unique_ptr<Client> &c) { return c->socket == socket; });
		bool wasActive = false;
		if (it != clients.end())
		{
			const Client &closing = **it;
			static const char *const protocols[] = {"undecided", "app", "http", "websocket"};
			debugLog(QStringLiteral("Closed connection from %1: %2, slot %3, \"%4\", open %5 s, %6 states, %7 bytes in, %8")
						 .arg(socket->peerAddress().toString(), QString::fromLatin1(protocols[static_cast<int>(closing.protocol)]))
						 .arg(closing.slot)
						 .arg(closing.name)
						 .arg((QDateTime::currentMSecsSinceEpoch() - closing.connectedAtMs) / 1000)
						 .arg(closing.states)
						 .arg(closing.bytesIn)
						 .arg(socket->errorString()));
			wasActive = (*it)->active;
			if ((*it)->executor)
				(*it)->executor->release_all();
			else if ((*it)->pad != nullptr)
				(*it)->pad->release_all();
			else if (wasActive && executor)
				executor->release_all();
			clients.erase(it);
		}
		socket->deleteLater();
		isGamepadConnected = std::any_of(clients.begin(), clients.end(),
										 [](const std::unique_ptr<Client> &c) { return c->active; });
		if (!isGamepadConnected)
			clientConnection = nullptr;
		if (wasActive)
		{
			qInfo() << "Device disconnected.";
			debugLog(QStringLiteral("Disconnected %1").arg(socket->peerAddress().toString()));
			qDebug() << "Average Request Interval" << averageRequestInterval
					 << "ms, Request Count:" << requestCount;
		}
		updateClientLabel();
		updateSlots();
	});
	connect(socket, &QAbstractSocket::readyRead, this, &Server::serveClient);
}

void Server::serveClient()
{
	auto *socket = qobject_cast<QTcpSocket *>(sender());
	const auto clientIt = std::find_if(clients.begin(), clients.end(),
									   [socket](const std::unique_ptr<Client> &c) { return c->socket == socket; });
	if (clientIt == clients.end())
		return;
	Client &client = **clientIt;
	const QByteArray incoming = socket->readAll();
	client.bytesIn += static_cast<quint64>(incoming.size());
	client.buffer.append(incoming);
	// Nothing legitimate is this large, so a client that sends it is cut off instead of filling memory
	if (client.buffer.size() > (1 << 20))
	{
		qWarning() << "Dropping" << socket->peerAddress().toString() << "after" << client.buffer.size() << "unread bytes";
		client.buffer.clear();
		socket->disconnectFromHost();
		return;
	}

	if (client.protocol == Client::Protocol::Unknown)
	{
		// Browsers speak HTTP and start with "GET ", the phone app starts with its own text or binary
		const QByteArray probe("GET ");
		if (client.buffer.size() < probe.size() && probe.startsWith(client.buffer))
			return;
		if (client.buffer.startsWith(probe))
		{
			client.protocol = Client::Protocol::Http;
		}
		else
		{
			client.protocol = Client::Protocol::Native;
			if (needsApproval(client))
			{
				askApproval(client);
				return;
			}
			QString failure;
			if (!activate(client, &failure))
			{
				refuse(client, failure);
				return;
			}
			socket->write("VGPSLOT:" + QByteArray::number(client.slot) + "\n");
		}
	}

	if (client.pending)
		return;
	switch (client.protocol)
	{
	case Client::Protocol::Native:
		serveNative(client);
		break;
	case Client::Protocol::Http:
		serveHttp(client);
		break;
	case Client::Protocol::WebSocket:
		serveWebSocket(client);
		break;
	default:
		break;
	}
}

void Server::serveNative(Client &client)
{
	ExecutorInterface *activeExecutor = executorFor(client);
	if (activeExecutor == nullptr)
		return;

	QTime currentTime = QTime::currentTime();

	// Process as many complete packets as we have in the buffer
	while (!client.buffer.isEmpty())
	{
		// A phone introduces itself first: VGPHELLO:<name>|<model>
		if (client.buffer.startsWith("VGPHELLO:"))
		{
			const int end = client.buffer.indexOf('\n');
			if (end < 0)
				break;
			const QStringList parts =
				QString::fromUtf8(client.buffer.mid(9, end - 9)).split('|');
			client.name = parts.value(0).trimmed();
			client.model = parts.value(1).trimmed();
			client.pingSupported = parts.value(2).trimmed() == QLatin1String("ping");
			client.buffer.remove(0, end + 1);
			qInfo() << "Device introduced itself:" << client.name << client.model;
			updateSlots();
			continue;
		}
		if (client.buffer.startsWith("VGPPONG:"))
		{
			const int end = client.buffer.indexOf('\n');
			if (end < 0)
				break;
			notePong(client, client.buffer.mid(8, end - 8).trimmed().toUInt());
			client.buffer.remove(0, end + 1);
			continue;
		}
		ParseResult result = parse_gamepad_state(client.buffer.constData(), client.buffer.size());

		if (!result.success)
		{
			switch (result.failure_reason)
			{
				using enum ParseResult::FailureReason;
			[[likely]] case IncompleteData:
				// Wait for more data
				break;
			case SchemaMismatch:
				qWarning() << "Schema mismatch detected in client data";
				client.buffer.remove(0, result.bytes_consumed); // Remove the processed data
				break;
			case DataTooLarge:
				qWarning() << "Client sent data that is too large to process";
				client.buffer.remove(0, result.bytes_consumed); // Remove the processed data
				break;
			[[unlikely]] default:
				qWarning() << "Unknown error occurred while parsing client data";
				client.buffer.clear(); // Clear buffer to avoid cascading errors
				break;
			}
			break; // Exit the processing loop
		}

		requestCount++;
		client.states++;
		if (lastRequestTime.isValid())
		{
			int elapsed = lastRequestTime.msecsTo(currentTime);
			averageRequestInterval += (elapsed - averageRequestInterval) / requestCount;
		}
		lastRequestTime = currentTime;

		noteInput(client, result.reading);
		activeExecutor->inject_gamepad_state(result.reading);
		client.buffer.remove(0, result.bytes_consumed);
	}
}

void Server::serveHttp(Client &client)
{
	const int end = client.buffer.indexOf("\r\n\r\n");
	if (end < 0)
	{
		if (client.buffer.size() > 16384)
			client.socket->disconnectFromHost();
		return;
	}
	const QByteArray head = client.buffer.left(end);
	client.buffer.remove(0, end + 4);

	const QList<QByteArray> lines = head.split('\n');
	const QByteArray path = lines.value(0).trimmed().split(' ').value(1);
	debugLog(QStringLiteral("HTTP %1 from %2").arg(QString::fromLatin1(lines.value(0).trimmed().left(200)), client.socket->peerAddress().toString()));
	QHash<QByteArray, QByteArray> headers;
	for (int i = 1; i < lines.size(); ++i)
	{
		const int colon = lines[i].indexOf(':');
		if (colon > 0)
			headers.insert(lines[i].left(colon).trimmed().toLower(), lines[i].mid(colon + 1).trimmed());
	}

	debugLog(QStringLiteral("Browser is %1").arg(QString::fromLatin1(headers.value("user-agent").left(300))));
	if (headers.value("upgrade").toLower() == "websocket" && headers.contains("sec-websocket-key"))
	{
		debugLog(QStringLiteral("WebSocket upgrade from %1").arg(client.socket->peerAddress().toString()));
		static const QByteArray magic("258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
		const QByteArray accept =
			QCryptographicHash::hash(headers.value("sec-websocket-key") + magic, QCryptographicHash::Sha1)
				.toBase64();
		client.socket->write("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
							 "Connection: Upgrade\r\nSec-WebSocket-Accept: " +
							 accept + "\r\n\r\n");
		client.protocol = Client::Protocol::WebSocket;
		if (needsApproval(client))
		{
			askApproval(client);
			return;
		}
		QString failure;
		if (!activate(client, &failure))
		{
			refuse(client, failure);
			return;
		}
		sendWebJson(client, QJsonObject{{"t", "slot"}, {"n", client.slot}, {"v", webPageVersion()}});
		serveWebSocket(client);
		return;
	}

	QByteArray response;
	if (path == "/" || path.startsWith("/?") || path == "/index.html")
	{
		QFile page(":/web/controller.html");
		QByteArray body = page.open(QIODevice::ReadOnly) ? page.readAll() : QByteArray();
		body.replace("__VGP_V__", webPageVersion().toUtf8());
		response = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: " +
				   QByteArray::number(body.size()) +
				   "\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n" + body;
	}
	else if (path == "/favicon.ico")
	{
		response = "HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n";
	}
	else
	{
		response = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
	}
	client.socket->write(response);
	client.socket->disconnectFromHost();
}

void Server::sendWebFrame(QTcpSocket *socket, const QByteArray &payload, int opcode)
{
	QByteArray frame;
	frame.append(static_cast<char>(0x80 | opcode));
	if (payload.size() < 126)
	{
		frame.append(static_cast<char>(payload.size()));
	}
	else
	{
		frame.append(static_cast<char>(126));
		frame.append(static_cast<char>((payload.size() >> 8) & 0xFF));
		frame.append(static_cast<char>(payload.size() & 0xFF));
	}
	frame.append(payload);
	socket->write(frame);
}

void Server::sendWebJson(Client &client, const QJsonObject &object)
{
	sendWebFrame(client.socket, QJsonDocument(object).toJson(QJsonDocument::Compact));
}

void Server::serveWebSocket(Client &client)
{
	while (client.buffer.size() >= 2)
	{
		const auto byteAt = [&client](int i) { return static_cast<quint8>(client.buffer[i]); };
		const int opcode = byteAt(0) & 0x0F;
		const bool masked = (byteAt(1) & 0x80) != 0;
		quint64 length = byteAt(1) & 0x7F;
		int position = 2;
		if (length == 126)
		{
			if (client.buffer.size() < 4)
				return;
			length = (static_cast<quint64>(byteAt(2)) << 8) | byteAt(3);
			position = 4;
		}
		else if (length == 127)
		{
			if (client.buffer.size() < 10)
				return;
			length = 0;
			for (int i = 0; i < 8; ++i)
				length = (length << 8) | byteAt(2 + i);
			position = 10;
		}
		if (!masked || length > 65536)
		{
			client.socket->disconnectFromHost();
			return;
		}
		if (static_cast<quint64>(client.buffer.size()) < static_cast<quint64>(position) + 4 + length)
			return;
		QByteArray payload = client.buffer.mid(position + 4, static_cast<int>(length));
		for (int i = 0; i < payload.size(); ++i)
			payload[i] = payload[i] ^ client.buffer[position + (i % 4)];
		client.buffer.remove(0, position + 4 + static_cast<int>(length));

		if (opcode == 0x1)
		{
			handleWebMessage(client, payload);
		}
		else if (opcode == 0x8)
		{
			sendWebFrame(client.socket, QByteArray(), 8);
			client.socket->disconnectFromHost();
			return;
		}
		else if (opcode == 0x9)
		{
			sendWebFrame(client.socket, payload, 0xA);
		}
	}
}

void Server::handleWebMessage(Client &client, const QByteArray &payload)
{
	const QJsonObject message = QJsonDocument::fromJson(payload).object();
	const QString type = message.value("t").toString();
	if (type == "hello")
	{
		client.name = message.value("name").toString().left(40).trimmed();
		client.model = message.value("model").toString().left(60).trimmed();
		debugLog(QStringLiteral("Hello from %1: %2, %3").arg(client.socket->peerAddress().toString(), client.name, client.model));
		updateSlots();
		return;
	}
	if (type == "pong")
	{
		notePong(client, static_cast<quint32>(message.value("id").toDouble()));
		return;
	}
	if (type == "dbg")
	{
		debugLog(QStringLiteral("Browser %1: %2").arg(client.socket->peerAddress().toString(), message.value("msg").toString().left(200)));
		return;
	}
	if (type != "state")
		return;

	ExecutorInterface *activeExecutor = executorFor(client);
	if (activeExecutor == nullptr)
		return;
	const auto axis = [&message](const char *key) {
		return static_cast<float>(std::clamp(message.value(key).toDouble(), -1.0, 1.0));
	};
	const auto trigger = [&message](const char *key) {
		return static_cast<float>(std::clamp(message.value(key).toDouble(), 0.0, 1.0));
	};
	vgp_data_exchange_gamepad_reading reading{};
	reading.buttons_down = static_cast<uint32_t>(message.value("d").toDouble()) & 0x3FFF;
	reading.buttons_up = static_cast<uint32_t>(message.value("u").toDouble()) & 0x3FFF;
	reading.left_thumbstick_x = axis("lx");
	reading.left_thumbstick_y = axis("ly");
	reading.right_thumbstick_x = axis("rx");
	reading.right_thumbstick_y = axis("ry");
	reading.left_trigger = trigger("lt");
	reading.right_trigger = trigger("rt");
	requestCount++;
	client.states++;
	noteInput(client, reading);
	activeExecutor->inject_gamepad_state(reading);
}

// Wrapped labels ask their parents for height-for-width, which made the scroll area size the whole page
// to the tallest wrapped layout. Their minimum heights are set explicitly, so opt out.
bool Server::eventFilter(QObject *watched, QEvent *event)
{
	if (watched == pagesHolder && event->type() == QEvent::Resize && pages != nullptr)
		pages->setGeometry(pagesHolder->rect());
	if (event->type() == QEvent::Resize)
	{
		for (SlotCard &card : slotCards)
		{
			if (watched == card.deviceIcon)
				refreshIcon(card);
		}
	}
	if (event->type() == QEvent::MouseButtonRelease)
	{
		for (const SlotCard &card : slotCards)
		{
			if (watched == card.frame && !card.occupied)
			{
				userPickedTab = true;
				showTab(1);
				break;
			}
		}
	}
	return QWidget::eventFilter(watched, event);
}

void Server::flattenLabels()
{
	const auto labels = findChildren<QLabel *>();
	for (QLabel *label : labels)
	{
		QSizePolicy policy = label->sizePolicy();
		if (policy.hasHeightForWidth())
		{
			policy.setHeightForWidth(false);
			label->setSizePolicy(policy);
		}
	}
}

void Server::updateQrPixmaps()
{
	for (size_t i = 0; i < qrLabels.size(); ++i)
	{
		const QString text = webQr ? tr("http://%1:%2/").arg(qrAddresses[i]).arg(tcpServer->serverPort())
								   : tr("%1:%2").arg(qrAddresses[i]).arg(tcpServer->serverPort());
		qrLabels[i]->setPixmap(QPixmap::fromImage(createQR(text)));
	}
	ui->qrCaption->setText(webQr ? tr("Scan with the phone camera to open the web controller")
								 : tr("Scan with the AirDeck app"));
	flattenLabels();
}

void Server::destroyServer()
{
	emit navigateBack();
	this->deleteLater();
}
