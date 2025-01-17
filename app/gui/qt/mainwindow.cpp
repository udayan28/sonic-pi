//--
// This file is part of Sonic Pi: http://sonic-pi.net
// Full project source: https://github.com/samaaron/sonic-pi
// License: https://github.com/samaaron/sonic-pi/blob/master/LICENSE.md
//
// Copyright 2013, 2014, 2015, 2016 by Sam Aaron (http://sam.aaron.name).
// All rights reserved.
//
// Permission is granted for use, copying, modification, and
// distribution of modified versions of this work as long as this
// notice is included.
//++


// Standard stuff
#include <iostream>
#include <math.h>
#include <sstream>
#include <fstream>

// Qt 5 only

// Qt stuff
#include <QDesktopWidget>
#include <QAction>
#include <QApplication>
#include <QFileDialog>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QDockWidget>
#include <QStatusBar>
#include <QTextBrowser>
#include <QToolBar>
#include <QShortcut>
#include <QToolButton>
#include <QScrollBar>
#include <QSplitter>
#include <QListWidget>
#include <QSplashScreen>
#include <QBoxLayout>
#include <QLabel>
#include <QLineEdit>

// QScintilla stuff
#include <Qsci/qsciapis.h>
#include <Qsci/qsciscintilla.h>

#include "widgets/sonicpilexer.h"
#include "widgets/sonicpiscintilla.h"
#include "utils/sonicpiapis.h"
#include "model/sonicpitheme.h"
#include "visualizer/scope.h"

// OSC stuff
#include "osc/oscpkt.hh"
#include "osc/udp.hh"
#include "osc/oschandler.h"
#include "osc/oscsender.h"
#include "osc/sonic_pi_udp_osc_server.h"
#include "osc/sonic_pi_tcp_osc_server.h"

#include "widgets/sonicpilog.h"
#include "widgets/infowidget.h"
#include "model/settings.h"
#include "widgets/settingswidget.h"

#include "utils/ruby_help.h"

using namespace oscpkt;// OSC specific stuff

// Operating System Specific includes
#if defined(Q_OS_WIN)
#include <QtConcurrent/QtConcurrentRun>
void sleep(int x) { Sleep((x)*1000); }
#elif defined(Q_OS_MAC)
#include <QtConcurrent/QtConcurrentRun>
#else
//assuming Raspberry Pi
#include <cmath>
#include <QtConcurrentRun>
#endif


#if QT_VERSION >= 0x050400
// Requires Qt5
#include <QWindow>
#endif

#include "mainwindow.h"

#ifdef Q_OS_MAC
MainWindow::MainWindow(QApplication &app, bool i18n, QMainWindow* splash)
#else
MainWindow::MainWindow(QApplication &app, bool i18n, QSplashScreen* splash)
#endif
{
    app.installEventFilter(this);
    app.processEvents();
    connect(&app, SIGNAL( aboutToQuit() ), this, SLOT( onExitCleanup() ) );

    printAsciiArtLogo();

    this->piSettings = new SonicPiSettings();

    this->splash = splash;
    this->i18n = i18n;

    sonicPiOSCServer = NULL;
    startup_error_reported = new QCheckBox;
    startup_error_reported->setChecked(false);

    hash_salt = "Secret Hash ;-)";

    protocol = UDP;
    if(protocol == TCP){
        clientSock = new QTcpSocket(this);
    }

    updated_dark_mode_for_help = false;
    updated_dark_mode_for_prefs = false;
    loaded_workspaces = false;
    is_recording = false;
    show_rec_icon_a = false;
    restoreDocPane = false;
    focusMode = false;
    version = "3.2.0";
    latest_version = "";
    version_num = 0;
    latest_version_num = 0;
    this->splash = splash;
    this->i18n = i18n;
    guiID = QUuid::createUuid().toString();
    QSettings settings("sonic-pi.net", "gui-settings");

    initPaths();

    // Throw all stdout into ~/.sonic-pi/log/gui.log
    setupLogPathAndRedirectStdOut();

    std::cout << "[GUI] - Welcome to the Sonic Pi GUI" << std::endl;
    std::cout << "[GUI] - ===========================" << std::endl;
    std::cout << "[GUI] -                            " << std::endl;
    std::cout << "[GUI] - " << guiID.toStdString() << std::endl;

    setupTheme();
    
    lexer = new SonicPiLexer(theme);
    QPalette p = theme->createPalette();
    QApplication::setPalette(p);

    // dynamically discover port numbers and then check them this will
    // show an error dialogue to the user and then kill the app if any of
    // the ports aren't available
    initAndCheckPorts();

    readSettings();
    oscSender = new OscSender(gui_send_to_server_port);

    QProcess *initProcess = new QProcess();
    initProcess->start(ruby_path, QStringList(init_script_path));

    setupWindowStructure();
    createStatusBar();
    createInfoPane();
    setWindowTitle(tr("Sonic Pi"));
    // Clear out old tasks from previous sessions if they still exist
    // in addtition to clearing out the logs

    initProcess->waitForFinished();
    startRubyServer();

    createShortcuts();
    createToolBar();
    updateTabsVisibility();
    updateButtonVisibility();
    updateLogVisibility();
    updateIncomingOscLogVisibility();
    // The implementation of this method is dynamically generated and can
    // be found in ruby_help.h:
    std::cout << "[GUI] - initialising documentation window" << std::endl;
    initDocsWindow();

    //setup autocompletion
    autocomplete->loadSamples(sample_path);

    OscHandler* handler = new OscHandler(this, outputPane, incomingPane, theme);

    if(protocol == UDP){
        sonicPiOSCServer = new SonicPiUDPOSCServer(this, handler, gui_listen_to_server_port);
        osc_thread = QtConcurrent::run(sonicPiOSCServer, &SonicPiOSCServer::start);
    }
    else{
        sonicPiOSCServer = new SonicPiTCPOSCServer(this, handler);
        sonicPiOSCServer->start();
    }

    QThreadPool::globalInstance()->setMaxThreadCount(3);
    //get their user email address from settings
    // user_token = new QLineEdit(this);

    // user_token->setText(settings.value("userToken", "").toString());
    std::cout << "[GUI] - honour prefs" << std::endl;
    restoreWindows();
    honourPrefs();
    std::cout << "[GUI] - update prefs icon" << std::endl;
    updatePrefsIcon();
    std::cout << "[GUI] - toggle icons" << std::endl;
    toggleIcons();
    std::cout << "[GUI] - full screen" << std::endl;

    updateFullScreenMode();

    std::cout << "[GUI] - hide" << std::endl;
    hide();
    // Wait to hear back from the Ruby language server before continuing
    std::cout << "[GUI] - wait for sync" << std::endl;

    if (waitForServiceSync()){
        // We have a connection! Finish up loading app...
        scopeInterface->scsynthBooted();
        updateColourTheme();
        std::cout << "[GUI] - load workspaces" << std::endl;
        loadWorkspaces();
        std::cout << "[GUI] - load request Version" << std::endl;
        requestVersion();
        changeSystemPreAmp(piSettings->main_volume, 1);

        QTimer *timer = new QTimer(this);
        connect(timer, SIGNAL(timeout()), this, SLOT(heartbeatOSC()));
        timer->start(1000);
        splashClose();
        showWindow();
        showWelcomeScreen();
        app.processEvents();
        std::cout << "[GUI] - boot sequence completed." << std::endl;

    } else {
        std::cout << "[GUI] - Critical Error. Unable to connect to server.." << std::endl;
        startupError("GUI was unable to connect to the Ruby server.");
    }

    app.setActiveWindow(tabs->currentWidget());

}

bool MainWindow::initAndCheckPorts() {
    std::cout << "[GUI] - Discovering port numbers..." << std::endl;

    QProcess* determinePortNumbers = new QProcess();
    QStringList determine_port_numbers_send_args;
    determine_port_numbers_send_args << port_discovery_path;
    determinePortNumbers->start(ruby_path, determine_port_numbers_send_args);
    determinePortNumbers->waitForFinished();
    QTextStream determine_port_numbers_stream(determinePortNumbers->readAllStandardOutput().trimmed());
    QString determine_port_numbers_line = determine_port_numbers_stream.readLine();
    while (!determine_port_numbers_line.isNull()) {
        auto parts = determine_port_numbers_line.split(": ");
        std::cout << "[GUI] - Port entry " << parts[0].trimmed().toStdString() << " : " << parts[1].trimmed().toStdString() << QString(" : %1").arg(parts[1].trimmed().toInt()).toStdString() << std::endl;
        port_map[parts[0].trimmed()] = parts[1].trimmed().toInt();
        determine_port_numbers_line = determine_port_numbers_stream.readLine();
    };

    gui_send_to_server_port   = port_map["gui-send-to-server"];
    gui_listen_to_server_port = port_map["gui-listen-to-server"];
    server_listen_to_gui_port = port_map["server-listen-to-gui"];
    server_osc_cues_port      = port_map["server-osc-cues"];
    server_send_to_gui_port   = port_map["server-send-to-gui"];
    scsynth_port              = port_map["scsynth"];
    scsynth_send_port         = port_map["scsynth-send"];
    erlang_router_port        = port_map["erlang-router"];
    osc_midi_out_port         = port_map["osc-midi-out"];
    osc_midi_in_port          = port_map["osc-midi-in"];
    websocket_port            = port_map["websocket"];

    std::cout << "[GUI] - Detecting port numbers..." << std::endl;


    std::cout << "[GUI] - GUI listen to server port "<< gui_listen_to_server_port << std::endl;
    bool glts_available = checkPort(gui_listen_to_server_port);

    std::cout << "[GUI] - Server listen to gui port " << server_listen_to_gui_port << std::endl;
    bool sltg_available = checkPort(server_listen_to_gui_port);

    std::cout << "[GUI] - Server incoming OSC cues port " << server_osc_cues_port << std::endl;
    bool soc_available = checkPort(server_osc_cues_port);

    std::cout << "[GUI] - Scsynth port " << scsynth_port << std::endl;
    bool s_available = checkPort(scsynth_port);

    std::cout << "[GUI] - Server send to GUI port " << server_send_to_gui_port << std::endl;
    bool sstg_available = checkPort(server_send_to_gui_port);

    std::cout << "[GUI] - GUI send to server port " << gui_send_to_server_port<< std::endl;
    bool gsts_available = checkPort(gui_send_to_server_port);

    std::cout << "[GUI] - Scsynth send port " << scsynth_send_port << std::endl;
    bool ss_available = checkPort(scsynth_send_port);

    std::cout << "[GUI] - Erlang router port " << erlang_router_port << std::endl;
    bool er_available = checkPort(erlang_router_port);

    std::cout << "[GUI] - OSC MIDI out port " << osc_midi_out_port << std::endl;
    bool omo_available = checkPort(osc_midi_out_port);

    std::cout << "[GUI] - OSC MIDI in port " << osc_midi_in_port << std::endl;
    bool omi_available = checkPort(osc_midi_in_port);

    std::cout << "[GUI] - Websocket port " << websocket_port << std::endl;
    bool ws_available = checkPort(websocket_port);

    if(!(glts_available &&
                sltg_available &&
                soc_available  &&
                s_available    &&
                sstg_available &&
                gsts_available &&
                ss_available   &&
                er_available   &&
                omo_available  &&
                omi_available   &&
                ws_available)){
        std::cout << "[GUI] - Critical Error. One or more ports is not available." << std::endl;
        startupError("One or more ports is not available. Is Sonic Pi already running? If not, please reboot your machine and try again.");
        return false;

    } else {
        std::cout << "[GUI] - All ports OK" << std::endl;
        return true;
    }

}


void MainWindow::initPaths() {
    QString root_path = rootPath();

#if defined(Q_OS_WIN)
    ruby_path = QDir::toNativeSeparators(root_path + "/app/server/native/ruby/bin/ruby.exe");
#elif defined(Q_OS_MAC)
    ruby_path = root_path + "/app/server/native/ruby/bin/ruby";
#else
    ruby_path = root_path + "/app/server/native/ruby/bin/ruby";
#endif

    QFile file(ruby_path);
    if(!file.exists()) {
        // fallback to user's locally installed ruby
        ruby_path = "ruby";
    }

    ruby_server_path = QDir::toNativeSeparators(root_path + "/app/server/ruby/bin/sonic-pi-server.rb");
    port_discovery_path = QDir::toNativeSeparators(root_path + "/app/server/ruby/bin/port-discovery.rb");
    fetch_url_path = QDir::toNativeSeparators(root_path + "/app/server/ruby/bin/fetch-url.rb");
    sample_path = QDir::toNativeSeparators(root_path + "/etc/samples");

    sp_user_path           = QDir::toNativeSeparators(sonicPiHomePath() + "/.sonic-pi");
    sp_user_tmp_path       = QDir::toNativeSeparators(sp_user_path + "/.writableTesterPath");
    log_path               = QDir::toNativeSeparators(sp_user_path + "/log");
    server_error_log_path  = QDir::toNativeSeparators(log_path + "/server-errors.log");
    server_output_log_path = QDir::toNativeSeparators(log_path + "/server-output.log");
    gui_log_path           = QDir::toNativeSeparators(log_path + QDir::separator() + "gui.log");
    process_log_path       = QDir::toNativeSeparators(log_path + "/processes.log");
    scsynth_log_path       = QDir::toNativeSeparators(log_path + QDir::separator() + "scsynth.log");

    init_script_path       = QDir::toNativeSeparators(root_path + "/app/server/ruby/bin/init-script.rb");
    exit_script_path       = QDir::toNativeSeparators(root_path + "/app/server/ruby/bin/exit-script.rb");

    qt_app_theme_path      = QDir::toNativeSeparators(root_path + "/app/gui/qt/theme/app.qss");

    qt_browser_dark_css    = QDir::toNativeSeparators(root_path + "/app/gui/qt/theme/dark/doc-styles.css");
    qt_browser_light_css   = QDir::toNativeSeparators(root_path + "/app/gui/qt/theme/light/doc-styles.css");
    qt_browser_hc_css      = QDir::toNativeSeparators(root_path + "/app/gui/qt/theme/high_contrast/doc-styles.css");

    // attempt to create log directory
    QDir logDir(log_path);
    logDir.mkpath(logDir.absolutePath());

    // check to see if the home directory is writable
    QFile tmpFile(sp_user_tmp_path);
    if (!tmpFile.open(QIODevice::WriteOnly)) {
        homeDirWritable = false;
    }
    else {
        homeDirWritable = true;
        tmpFile.close();
    }

}


void MainWindow::checkForStudioMode() {
    // Studio mode should always be enabled on linux
#if defined(Q_OS_LINUX)
    studio_mode->setChecked(true);
    return;
#else
    // other operating systems need to support the project
    //to enable studio mode
    studio_mode->setChecked(false);
#endif

    QString queryStr;
    queryStr = QString("%1")
        .arg(QString(QCryptographicHash::hash(QString(user_token->text() + hash_salt).toUtf8(),QCryptographicHash::Sha256).toHex()));


    QStringList studioHashList = QStringList();

    std::cout << "[GUI] - Fetching Studio hashes" << std::endl;
    QProcess* fetchStudioHashes = new QProcess();
    QStringList fetch_studio_hashes_send_args;
    fetch_studio_hashes_send_args << fetch_url_path << "http://sonic-pi.net/static/info/studio-hashes.txt";
    fetchStudioHashes->start(ruby_path, fetch_studio_hashes_send_args);
    fetchStudioHashes->waitForFinished();
    QTextStream stream(fetchStudioHashes->readAllStandardOutput().trimmed());
    QString line = stream.readLine();
    while (!line.isNull()) {
        studioHashList << line;
        line = stream.readLine();
    };

    if(studioHashList.contains(queryStr)) {
        std::cout << "[GUI] - Found Studio Hash Match" << std::endl;
        std::cout << "[GUI] - Enabling Studio Mode..." << std::endl;
        std::cout << "[GUI] - Thank-you for supporting Sonic Pi's continued development :-)" << std::endl;
        statusBar()->showMessage(tr("Studio Mode Enabled. Thank-you for supporting Sonic Pi."), 5000);
        studio_mode->setChecked(true);
    } else {
        std::cout << "[GUI] - No Studio Hash Match Found" << std::endl;
        statusBar()->showMessage(tr("No Matching Studio Hash Found..."), 1000);
        studio_mode->setChecked(false);
    }
}

bool MainWindow::checkPort(int port) {
    bool available = false;
    oscpkt::UdpSocket sock;
    sock.bindTo(port);
    if ((port < 1024) || (!sock.isOk())) {
        std::cout << "[GUI] -    port: " << port << " [Not Available]" << std::endl;
        available = false;
    } else {
        std::cout << "[GUI] -    port: " << port << " [OK]" << std::endl;
        available = true;
    }
    sock.close();
    return available;
}

void MainWindow::showWelcomeScreen() {
    QSettings settings("sonic-pi.net", "gui-settings");
    if(settings.value("first_time", 1).toInt() == 1) {
        QTextBrowser* startupPane = new QTextBrowser;
        startupPane->setFixedSize(600, 615);
        startupPane->setWindowIcon(QIcon(":images/icon-smaller.png"));
        startupPane->setWindowTitle(tr("Welcome to Sonic Pi"));
        addUniversalCopyShortcuts(startupPane);
        startupPane->document()->setDefaultStyleSheet(readFile(":/theme/light/doc-styles.css"));
        startupPane->setSource(QUrl("qrc:///html/startup.html"));
        docWidget->show();
        startupPane->show();
    }
}

void MainWindow::setupTheme() {
    // Syntax highlighting
    QString themeFilename = QDir::homePath() + QDir::separator() + ".sonic-pi" + QDir::separator() + "theme.properties";
    this->theme = new SonicPiTheme(this, themeFilename, rootPath());
}

void MainWindow::setupWindowStructure() {
    std::cout << "[GUI] - setting up window structure" << std::endl;

    setUnifiedTitleAndToolBarOnMac(true);
    setWindowIcon(QIcon(":images/icon-smaller.png"));

    rec_flash_timer = new QTimer(this);
    connect(rec_flash_timer, SIGNAL(timeout()), this, SLOT(toggleRecordingOnIcon()));

    // Setup output and error panes

    outputPane = new SonicPiLog;
    incomingPane = new SonicPiLog;
    errorPane = new QTextBrowser;
    errorPane->setOpenExternalLinks(true);

    // Window layout
    tabs = new QTabWidget();
    tabs->setTabsClosable(false);
    tabs->setMovable(false);
    tabs->setTabPosition(QTabWidget::South);

    lexer->setAutoIndentStyle(SonicPiScintilla::AiMaintain);

    // create workspaces and add them to the tabs
    // workspace shortcuts
    signalMapper = new QSignalMapper (this) ;

    prefsWidget = new QDockWidget(tr("Preferences"), this);
    prefsWidget->setFocusPolicy(Qt::NoFocus);
    prefsWidget->setAllowedAreas(Qt::RightDockWidgetArea);
    prefsWidget->setFeatures(QDockWidget::DockWidgetClosable);

    settingsWidget = new SettingsWidget(server_osc_cues_port, piSettings, this);
    connect(settingsWidget, SIGNAL(volumeChanged(int)), this, SLOT(changeSystemPreAmp(int)));
    connect(settingsWidget, SIGNAL(mixerSettingsChanged()), this, SLOT(mixerSettingsChanged()));
    connect(settingsWidget, SIGNAL(midiSettingsChanged()), this, SLOT(toggleMidi()));
    connect(settingsWidget, SIGNAL(resetMidi()), this, SLOT(resetMidi()));
    connect(settingsWidget, SIGNAL(oscSettingsChanged()), this, SLOT(toggleOSCServer()));
    connect(settingsWidget, SIGNAL(showLineNumbersChanged()), this, SLOT(changeShowLineNumbers()));
    connect(settingsWidget, SIGNAL(showLogChanged()), this, SLOT(updateLogVisibility()));
    connect(settingsWidget, SIGNAL(incomingOscLogChanged()), this, SLOT(updateIncomingOscLogVisibility()));
    connect(settingsWidget, SIGNAL(showButtonsChanged()), this, SLOT(updateButtonVisibility()));
    connect(settingsWidget, SIGNAL(showFullscreenChanged()), this, SLOT(updateFullScreenMode()));
    connect(settingsWidget, SIGNAL(showTabsChanged()), this, SLOT(updateTabsVisibility()));
    connect(settingsWidget, SIGNAL(logAutoScrollChanged()), this, SLOT(updateLogAutoScroll()));
    connect(settingsWidget, SIGNAL(themeChanged()), this, SLOT(updateColourTheme()));
    connect(settingsWidget, SIGNAL(scopeChanged()), this, SLOT(scope()));
    connect(settingsWidget, SIGNAL(scopeChanged(QString)), this, SLOT(toggleScope(QString)));
    connect(settingsWidget, SIGNAL(scopeAxesChanged()), this, SLOT(toggleScopeAxes()));
    connect(settingsWidget, SIGNAL(transparencyChanged(int)), this, SLOT(changeGUITransparency(int)));

    connect(settingsWidget, SIGNAL(checkUpdatesChanged()), this, SLOT(update_check_updates()));
    connect(settingsWidget, SIGNAL(forceCheckUpdates()), this, SLOT(check_for_updates_now()));

    connect(this, SIGNAL(settingsChanged()), settingsWidget, SLOT(settingsChanged()));  

    scopeInterface = new Scope(scsynth_port);
    scopeInterface->pause();
    restoreScopeState(scopeInterface->getScopeNames());
    settingsWidget->updateScopeNames(scopeInterface->getScopeNames());

    prefsCentral = new QWidget;
    prefsCentral->setObjectName("prefsCentral");
    prefsWidget->setWidget(settingsWidget);
    QSizePolicy prefsSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    prefsCentral->setSizePolicy(prefsSizePolicy);
    addDockWidget(Qt::RightDockWidgetArea, prefsWidget);
    prefsWidget->hide();
    prefsWidget->setObjectName("prefs");

    connect(prefsWidget, SIGNAL(visibilityChanged(bool)), this, SLOT(updatePrefsIcon()));
    bool auto_indent = piSettings->auto_indent_on_run;
    for(int ws = 0; ws < workspace_max; ws++) {
        std::string s;
        QString fileName = QString("workspace_" ) + QString::fromStdString(number_name(ws));

        //TODO: this is only here to ensure auto_indent_on_run is
        //      initialised before using it to construct the
        //      workspaces. Strongly consider how to clean this up in a way
        //      that nicely scales for more properties such as this.  This
        //      should only be considered an interim solution necessary to
        //      fix the return issue on Japanese keyboards.

        SonicPiScintilla *workspace = new SonicPiScintilla(lexer, theme, fileName, oscSender, auto_indent);

        workspace->setObjectName(QString("Buffer %1").arg(ws));

        //tab completion when in list
        QShortcut *indentLine = new QShortcut(QKeySequence("Tab"), workspace);
        connect (indentLine, SIGNAL(activated()), signalMapper, SLOT(map())) ;
        signalMapper -> setMapping (indentLine, (QObject*)workspace);

        // save and load buffers
        QShortcut *saveBufferShortcut = new QShortcut(shiftMetaKey('s'), workspace);
        connect (saveBufferShortcut, SIGNAL(activated()), this, SLOT(saveAs())) ;
        QShortcut *loadBufferShortcut = new QShortcut(shiftMetaKey('o'), workspace);
        connect (loadBufferShortcut, SIGNAL(activated()), this, SLOT(loadFile())) ;


        //transpose chars
        QShortcut *transposeChars = new QShortcut(ctrlKey('t'), workspace);
        connect (transposeChars, SIGNAL(activated()), workspace, SLOT(transposeChars())) ;

        //move line or selection up and down
        QShortcut *moveLineUp = new QShortcut(ctrlMetaKey('p'), workspace);
        connect (moveLineUp, SIGNAL(activated()), workspace, SLOT(moveLineOrSelectionUp())) ;

        QShortcut *moveLineDown = new QShortcut(ctrlMetaKey('n'), workspace);
        connect (moveLineDown, SIGNAL(activated()), workspace, SLOT(moveLineOrSelectionDown())) ;

        // Contextual help
        QShortcut *contextHelp = new QShortcut(ctrlKey('i'), workspace);
        connect (contextHelp, SIGNAL(activated()), this, SLOT(helpContext()));

        QShortcut *contextHelp2 = new QShortcut(QKeySequence("F1"), workspace);
        connect (contextHelp2, SIGNAL(activated()), this, SLOT(helpContext()));


        // Font zooming
        QShortcut *fontZoom = new QShortcut(metaKey('='), workspace);
        connect (fontZoom, SIGNAL(activated()), workspace, SLOT(zoomFontIn()));

        QShortcut *fontZoom2 = new QShortcut(metaKey('+'), workspace);
        connect (fontZoom2, SIGNAL(activated()), workspace, SLOT(zoomFontIn()));


        QShortcut *fontZoomOut = new QShortcut(metaKey('-'), workspace);
        connect (fontZoomOut, SIGNAL(activated()), workspace, SLOT(zoomFontOut()));

        QShortcut *fontZoomOut2 = new QShortcut(metaKey('_'), workspace);
        connect (fontZoomOut2, SIGNAL(activated()), workspace, SLOT(zoomFontOut()));

        //set Mark
#ifdef Q_OS_MAC
        QShortcut *setMark = new QShortcut(QKeySequence("Meta+Space"), workspace);
#else
        QShortcut *setMark = new QShortcut(QKeySequence("Ctrl+Space"), workspace);
#endif
        connect (setMark, SIGNAL(activated()), workspace, SLOT(setMark())) ;

        //escape
        QShortcut *escape = new QShortcut(ctrlKey('g'), workspace);
        QShortcut *escape2 = new QShortcut(QKeySequence("Escape"), workspace);
        connect(escape, SIGNAL(activated()), this, SLOT(escapeWorkspaces()));
        connect(escape2, SIGNAL(activated()), this, SLOT(escapeWorkspaces()));

        //quick nav by jumping up and down 1 lines at a time
        QShortcut *forwardOneLine = new QShortcut(ctrlKey('p'), workspace);
        connect(forwardOneLine, SIGNAL(activated()), workspace, SLOT(forwardOneLine()));
        QShortcut *backOneLine = new QShortcut(ctrlKey('n'), workspace);
        connect(backOneLine, SIGNAL(activated()), workspace, SLOT(backOneLine()));

        //quick nav by jumping up and down 10 lines at a time
        QShortcut *forwardTenLines = new QShortcut(shiftMetaKey('u'), workspace);
        connect(forwardTenLines, SIGNAL(activated()), workspace, SLOT(forwardTenLines()));
        QShortcut *backTenLines = new QShortcut(shiftMetaKey('d'), workspace);
        connect(backTenLines, SIGNAL(activated()), workspace, SLOT(backTenLines()));

        //cut to end of line
        QShortcut *cutToEndOfLine = new QShortcut(ctrlKey('k'), workspace);
        connect(cutToEndOfLine, SIGNAL(activated()), workspace, SLOT(cutLineFromPoint()));

        //Emacs live copy and cut
        QShortcut *copyToBuffer = new QShortcut(metaKey(']'), workspace);
        connect(copyToBuffer, SIGNAL(activated()), workspace, SLOT(copyClear()));

        QShortcut *cutToBufferLive = new QShortcut(ctrlKey(']'), workspace);
        connect(cutToBufferLive, SIGNAL(activated()), workspace, SLOT(sp_cut()));

        // Standard cut
        QShortcut *cutToBuffer = new QShortcut(ctrlKey('x'), workspace);
        connect(cutToBuffer, SIGNAL(activated()), workspace, SLOT(sp_cut()));

        // paste
        QShortcut *pasteToBufferWin = new QShortcut(ctrlKey('v'), workspace);
        connect(pasteToBufferWin, SIGNAL(activated()), workspace, SLOT(sp_paste()));
        QShortcut *pasteToBuffer = new QShortcut(metaKey('v'), workspace);
        connect(pasteToBuffer, SIGNAL(activated()), workspace, SLOT(sp_paste()));
        QShortcut *pasteToBufferEmacs = new QShortcut(ctrlKey('y'), workspace);
        connect(pasteToBufferEmacs, SIGNAL(activated()), workspace, SLOT(sp_paste()));

        //comment line
        QShortcut *toggleLineComment= new QShortcut(metaKey('/'), workspace);
        connect(toggleLineComment, SIGNAL(activated()), this, SLOT(toggleCommentInCurrentWorkspace()));

        //upcase next word
        QShortcut *upcaseWord= new QShortcut(metaKey('u'), workspace);
        connect(upcaseWord, SIGNAL(activated()), workspace, SLOT(upcaseWordOrSelection()));

        //downcase next word
        QShortcut *downcaseWord= new QShortcut(metaKey('l'), workspace);
        connect(downcaseWord, SIGNAL(activated()), workspace, SLOT(downcaseWordOrSelection()));

        QString w = QString(tr("| %1 |")).arg(QString::number(ws));
        workspaces[ws] = workspace;
        tabs->addTab(workspace, w);
    }

    connect(signalMapper, SIGNAL(mapped(int)), this, SLOT(changeTab(int)));
    connect(signalMapper, SIGNAL(mapped(QObject*)), this, SLOT(completeSnippetListOrIndentLine(QObject*)));

    QFont font("Monospace");
    font.setStyleHint(QFont::Monospace);
    lexer->setDefaultFont(font);

    autocomplete = new SonicPiAPIs(lexer);
    // adding universal shortcuts to outputpane seems to
    // steal events from doc system!?
    // addUniversalCopyShortcuts(outputPane);
#if QT_VERSION >= 0x050400
    //requires Qt 5
    new QShortcut(ctrlKey('='), this, SLOT(zoomInLogs()));
    new QShortcut(ctrlKey('-'), this, SLOT(zoomOutLogs()));

#endif
    addUniversalCopyShortcuts(errorPane);
    outputPane->setReadOnly(true);
    incomingPane->setReadOnly(true);
    errorPane->setReadOnly(true);
    outputPane->setLineWrapMode(QPlainTextEdit::NoWrap);
    outputPane->setFontFamily("Hack");
    incomingPane->setLineWrapMode(QPlainTextEdit::NoWrap);
    incomingPane->setFontFamily("Hack");

    if(!theme->font("LogFace").isEmpty()){
        outputPane->setFontFamily(theme->font("LogFace"));
        incomingPane->setFontFamily(theme->font("LogFace"));
    }

    outputPane->document()->setMaximumBlockCount(1000);
    incomingPane->document()->setMaximumBlockCount(1000);
    errorPane->document()->setMaximumBlockCount(1000);

    outputPane->setTextColor(QColor(theme->color("LogForeground")));
    outputPane->appendPlainText("\n");
    incomingPane->setTextColor(QColor(theme->color("LogForeground")));
    incomingPane->appendPlainText("\n");

    errorPane->zoomIn(1);
    errorPane->setMaximumHeight(130);
    errorPane->setMinimumHeight(130);

    // hudPane = new QTextBrowser;
    // hudPane->setMinimumHeight(130);
    // hudPane->setHtml("<center><img src=\":/images/logo.png\" height=\"113\" width=\"138\"></center>");
    // hudWidget = new QDockWidget(this);
    // hudWidget->setFeatures(QDockWidget::NoDockWidgetFeatures);
    // hudWidget->setAllowedAreas(Qt::RightDockWidgetArea);
    // hudWidget->setTitleBarWidget(new QWidget());
    // addDockWidget(Qt::RightDockWidgetArea, hudWidget);
    // hudWidget->setWidget(hudPane);
    // hudWidget->setObjectName("hud");

    scopeWidget = new QDockWidget("",this);
    scopeWidget->setFocusPolicy(Qt::NoFocus);
    scopeWidget->setAllowedAreas(Qt::RightDockWidgetArea | Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);
    scopeWidget->setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    scopeWidget->setWidget(scopeInterface);
    scopeWidget->setObjectName("Scope");
    addDockWidget(Qt::RightDockWidgetArea, scopeWidget);

    connect(scopeWidget, SIGNAL(visibilityChanged(bool)), this, SLOT(scopeVisibilityChanged()));


    outputWidget = new QDockWidget(tr("Log"), this);
    outputWidget->setFocusPolicy(Qt::NoFocus);
    outputWidget->setFeatures(QDockWidget::NoDockWidgetFeatures);
    outputWidget->setAllowedAreas(Qt::RightDockWidgetArea);
    outputWidget->setWidget(outputPane);

    incomingWidget = new QDockWidget(tr("Cues"), this);
    incomingWidget->setFocusPolicy(Qt::NoFocus);
    incomingWidget->setFeatures(QDockWidget::NoDockWidgetFeatures);
    incomingWidget->setAllowedAreas(Qt::RightDockWidgetArea);
    incomingWidget->setWidget(incomingPane);

    addDockWidget(Qt::RightDockWidgetArea, outputWidget);
    addDockWidget(Qt::RightDockWidgetArea, incomingWidget);
    outputWidget->setObjectName("output");
    incomingWidget->setObjectName("input");

    blankWidget = new QWidget();
    outputWidgetTitle = outputWidget->titleBarWidget();

    docsCentral = new QTabWidget;
    docsCentral->setFocusPolicy(Qt::NoFocus);
    docsCentral->setTabsClosable(false);
    docsCentral->setMovable(false);
    docsCentral->setTabPosition(QTabWidget::South);

    docPane = new QTextBrowser;
    QSizePolicy policy = docPane->sizePolicy();
    policy.setHorizontalStretch(QSizePolicy::Maximum);
    docPane->setSizePolicy(policy);
    docPane->setMinimumHeight(200);
    docPane->setOpenExternalLinks(true);

    QShortcut *up = new QShortcut(ctrlKey('p'), docPane);
    up->setContext(Qt::WidgetShortcut);
    connect(up, SIGNAL(activated()), this, SLOT(docScrollUp()));
    QShortcut *down = new QShortcut(ctrlKey('n'), docPane);
    down->setContext(Qt::WidgetShortcut);
    connect(down, SIGNAL(activated()), this, SLOT(docScrollDown()));

    docPane->setSource(QUrl("qrc:///html/doc.html"));

    addUniversalCopyShortcuts(docPane);

    docsplit = new QSplitter;

    docsplit->addWidget(docsCentral);
    docsplit->addWidget(docPane);

    docWidget = new QDockWidget(tr("Help"), this);
    docWidget->setFocusPolicy(Qt::NoFocus);
    docWidget->setAllowedAreas(Qt::BottomDockWidgetArea);
    docWidget->setWidget(docsplit);
    docWidget->setObjectName("help");

    addDockWidget(Qt::BottomDockWidgetArea, docWidget);
    docWidget->hide();

    //Currently causes a segfault when dragging doc pane out of main
    //window:
    connect(docWidget, SIGNAL(visibilityChanged(bool)), this, SLOT(toggleHelpIcon()));

    mainWidgetLayout = new QVBoxLayout;
    mainWidgetLayout->addWidget(tabs);
    mainWidgetLayout->addWidget(errorPane);
    mainWidgetLayout->setMargin(0);
    mainWidget = new QWidget;
    mainWidget->setFocusPolicy(Qt::NoFocus);
    errorPane->hide();
    mainWidget->setLayout(mainWidgetLayout);
    mainWidget->setObjectName("mainWidget");
    setCentralWidget(mainWidget);

}

void MainWindow::escapeWorkspaces() {
    resetErrorPane();

    for (int w=0; w < workspace_max; w++) {
        workspaces[w]->escapeAndCancelSelection();
        workspaces[w]->clearLineMarkers();
    }
}

void MainWindow::changeTab(int id){
    tabs->setCurrentIndex(id);
}

void MainWindow::toggleFullScreenMode() {
    piSettings->full_screen = !piSettings->full_screen;
    emit settingsChanged();
    updateFullScreenMode();
}

void MainWindow::updateFullScreenMode(){
    if (piSettings->full_screen) {
        outputWidget->setTitleBarWidget(blankWidget);
#ifdef Q_OS_WIN
        this->setWindowFlags(Qt::FramelessWindowHint);
#endif
        int currentScreen = QApplication::desktop()->screenNumber(this);
        statusBar()->showMessage(tr("Full screen mode on."), 2000);
#if QT_VERSION >= 0x050400
        //requires Qt5
        this->windowHandle()->setScreen(qApp->screens()[currentScreen]);
#endif
        this->setWindowState(Qt::WindowFullScreen);
        this->show();
    }
    else {
        outputWidget->setTitleBarWidget(outputWidgetTitle);
        this->setWindowState(windowState() & ~(Qt::WindowFullScreen));
#ifdef Q_OS_WIN
        this->setWindowFlags(Qt::WindowTitleHint | Qt::WindowSystemMenuHint |
                Qt::WindowMinimizeButtonHint |
                Qt::WindowMaximizeButtonHint |
                Qt::WindowCloseButtonHint);
#endif
        statusBar()->showMessage(tr("Full screen mode off."), 2000);
        this->show();
    }
}

void MainWindow::toggleFocusMode() {
    focusMode = !focusMode;
    updateFocusMode();
}

void MainWindow::updateFocusMode(){
    if (focusMode) {
        piSettings->full_screen = true;
        piSettings->show_tabs = false;
        piSettings->show_buttons = false;
        piSettings->show_log = false;
        piSettings->show_incoming_osc_log = false;
    }
    else {
        piSettings->full_screen = false;
        piSettings->show_tabs = true;
        piSettings->show_buttons = true;
        piSettings->show_incoming_osc_log = true;
    }
    emit settingsChanged();
    updateFullScreenMode();
    updateTabsVisibility();
    updateButtonVisibility();
    updateLogVisibility();
    updateIncomingOscLogVisibility();
}

void MainWindow::toggleScopePaused() {
    scopeInterface->togglePause();
}

void MainWindow::allJobsCompleted() {
    scopeInterface->pause();

    // re-enable log text selection
    incomingPane->setTextInteractionFlags(Qt::TextSelectableByMouse);
    outputPane->setTextInteractionFlags(Qt::TextSelectableByMouse);

}

void MainWindow::toggleLogVisibility() {
    piSettings->show_log = !piSettings->show_log;
    emit settingsChanged();
    updateLogVisibility();
}

void MainWindow::updateLogVisibility(){
    if(piSettings->show_log) {
        outputWidget->show();
    } else{
        outputWidget->close();
    }
}

void MainWindow::updateIncomingOscLogVisibility(){
    if(piSettings->show_incoming_osc_log) {
        incomingWidget->show();
    } else{
        incomingWidget->close();
    }
}

void MainWindow::toggleTabsVisibility() {
    piSettings->show_tabs = !piSettings->show_tabs;
    emit settingsChanged();
    updateTabsVisibility();
}

void MainWindow::updateTabsVisibility(){
    QTabBar *tabBar = tabs->findChild<QTabBar *>();

    if(piSettings->show_tabs) {
        tabBar->show();
    }
    else{
        tabBar->hide();
    }
}

void MainWindow::toggleButtonVisibility() {
    piSettings->show_buttons = !piSettings->show_buttons;
    emit settingsChanged();
    updateButtonVisibility();
}

void MainWindow::updateButtonVisibility(){
    if (piSettings->show_buttons) {
        toolBar->show();
    }
    else {
        toolBar->close();
    }
}

void MainWindow::completeSnippetListOrIndentLine(QObject* ws){
    SonicPiScintilla *spws = ((SonicPiScintilla*)ws);
    if(spws->isListActive()) {
        spws->tabCompleteifList();
    }
    else {
        completeSnippetOrIndentCurrentLineOrSelection(spws);
    }
}

void MainWindow::completeSnippetOrIndentCurrentLineOrSelection(SonicPiScintilla* ws) {
    int start_line, finish_line, point_line, point_index;
    ws->getCursorPosition(&point_line, &point_index);
    if(ws->hasSelectedText()) {
        statusBar()->showMessage(tr("Indenting selection..."), 2000);
        int unused_a, unused_b;
        ws->getSelection(&start_line, &unused_a, &finish_line, &unused_b);
    } else {
        statusBar()->showMessage(tr("Indenting line..."), 2000);
        start_line = point_line;
        finish_line = point_line;
    }


    std::string code = ws->text().toStdString();

    Message msg("/buffer-section-complete-snippet-or-indent-selection");
    msg.pushStr(guiID.toStdString());
    std::string filename = ws->fileName.toStdString();
    msg.pushStr(filename);
    msg.pushStr(code);
    msg.pushInt32(start_line);
    msg.pushInt32(finish_line);
    msg.pushInt32(point_line);
    msg.pushInt32(point_index);
    sendOSC(msg);
}

void MainWindow::toggleCommentInCurrentWorkspace() {
    SonicPiScintilla *ws = (SonicPiScintilla*)tabs->currentWidget();
    toggleComment(ws);
}

void MainWindow::toggleComment(SonicPiScintilla* ws) {
    int start_line, finish_line, point_line, point_index;
    ws->getCursorPosition(&point_line, &point_index);
    if(ws->hasSelectedText()) {
        statusBar()->showMessage(tr("Toggle selection comment..."), 2000);
        int unused_a, unused_b;
        ws->getSelection(&start_line, &unused_a, &finish_line, &unused_b);
    } else {
        statusBar()->showMessage(tr("Toggle line comment..."), 2000);
        start_line = point_line;
        finish_line = point_line;
    }


    std::string code = ws->text().toStdString();

    Message msg("/buffer-section-toggle-comment");
    msg.pushStr(guiID.toStdString());
    std::string filename = ws->fileName.toStdString();
    msg.pushStr(filename);
    msg.pushStr(code);
    msg.pushInt32(start_line);
    msg.pushInt32(finish_line);
    msg.pushInt32(point_line);
    msg.pushInt32(point_index);
    sendOSC(msg);
}

QString MainWindow::rootPath() {
    // diversity is the spice of life
#if defined(Q_OS_MAC)
    return QCoreApplication::applicationDirPath() + "/../Resources";
#elif defined(Q_OS_WIN)
    return QCoreApplication::applicationDirPath() + "/../../../..";
#else
    return QCoreApplication::applicationDirPath() + "/../../..";
#endif
}

void MainWindow::startRubyServer(){

    // kill any zombie processes that may exist
    // better: test to see if UDP ports are in use, only kill/sleep if so
    // best: kill SCSynth directly if needed
    serverProcess = new QProcess();

    QStringList args;
#if defined(Q_OS_MAC)
    args << "--enable-frozen-string-literal";
#elif defined(Q_OS_WIN)
    args << "--enable-frozen-string-literal";
#endif

    args << "-E" << "utf-8";
    args << ruby_server_path;


    if(protocol == TCP){
        args << "-t";
    }else {
        args << "-u";
    }


    args <<
        QString("%1").arg(server_listen_to_gui_port) <<
        QString("%1").arg(server_send_to_gui_port) <<
        QString("%1").arg(scsynth_port) <<
        QString("%1").arg(scsynth_send_port) <<
        QString("%1").arg(server_osc_cues_port) <<
        QString("%1").arg(erlang_router_port) <<
        QString("%1").arg(osc_midi_out_port) <<
        QString("%1").arg(osc_midi_in_port) <<
        QString("%1").arg(websocket_port);;
    std::cout << "[GUI] - launching Sonic Pi Runtime Server:" << std::endl;
    if(homeDirWritable) {
        serverProcess->setStandardErrorFile(server_error_log_path);
        serverProcess->setStandardOutputFile(server_output_log_path);
    }
    serverProcess->start(ruby_path, args);
    // Register server pid for potential zombie clearing
    QStringList regServerArgs;
#if QT_VERSION >= QT_VERSION_CHECK(5, 3, 0)
    regServerArgs << QDir::toNativeSeparators(rootPath() + "/app/server/ruby/bin/task-register.rb")<< QString::number(serverProcess->processId());
#endif
    QProcess *regServerProcess = new QProcess();
    regServerProcess->start(ruby_path, regServerArgs);
    regServerProcess->waitForFinished();
#if QT_VERSION >= QT_VERSION_CHECK(5, 3, 0)
    std::cout << "[GUI] - Ruby server pid registered: "<< serverProcess->processId() << std::endl;
#endif

    if (!serverProcess->waitForStarted()) {
        invokeStartupError(tr("The Sonic Pi Server could not be started!"));
        return;
    }
}

bool MainWindow::waitForServiceSync() {
    QString contents;
    std::cout << "[GUI] - waiting for Sonic Pi Server to boot..." << std::endl;
    bool server_booted = false;
    if (!homeDirWritable) {
        // we can't monitor the logs so hope for the best!
        sleep(15);
        server_booted = true;
    } else {
        for(int i = 0; i < 60; i ++) {
            qApp->processEvents();
            contents = readFile(server_output_log_path);
            if (contents.contains("Sonic Pi Server successfully booted.")) {
                std::cout << std::endl << "[GUI] - Sonic Pi Server successfully booted." << std::endl;
                server_booted = true;
                break;
            } else {
                std::cout << ".";
                sleep(1);
            }
        }
    }

    if (!server_booted) {
        std::cout << std::endl << "[GUI] - Critical error! Could not boot Sonic Pi Server." << std::endl;
        invokeStartupError("Critical error! - Could not boot Sonic Pi Server.");
        return false;
    }

    int timeout = 60;
    std::cout << "[GUI] - waiting for Sonic Pi Server to respond..." << std::endl;
    while (sonicPiOSCServer->waitForServer() && timeout-- > 0) {
        sleep(1);
        std::cout << ".";
        if(sonicPiOSCServer->isIncomingPortOpen()) {
            Message msg("/ping");
            msg.pushStr(guiID.toStdString());
            msg.pushStr("QtClient/1/hello");
            sendOSC(msg);
        }
    }
    if (!sonicPiOSCServer->isServerStarted()) {
        std::cout << std::endl <<  "[GUI] - Critical error! Could not connect to Sonic Pi Server." << std::endl;
        invokeStartupError("Critical server error - could not connect to Sonic Pi Server!");
        return false;
    } else {
        std::cout << std::endl << "[GUI] - Sonic Pi Server connection established" << std::endl;
        return true;
    }

}

void MainWindow::splashClose() {
#if defined(Q_OS_MAC)
    splash->close();
#else
    splash->finish(this);
#endif
}

void MainWindow::showWindow() {
    QSettings settings("sonic-pi.net", "gui-settings");
    if(settings.value("first_time", 1).toInt() == 1) {
        showMaximized();
    } else {
        showNormal();

    }
    changeShowLineNumbers();
}

void MainWindow::mixerSettingsChanged() {
    std::cout << "Mixer Settings Changed!" << std::endl;
    if (piSettings->mixer_invert_stereo) {
        mixerInvertStereo();
    } else {
        mixerStandardStereo();
    }

    if (piSettings->mixer_force_mono) {
        mixerMonoMode();
    } else {
        mixerStereoMode();
    }
}

void MainWindow::update_check_updates() {
    if (piSettings->check_updates) {
        enableCheckUpdates();
    } else {
        disableCheckUpdates();
    }
}

bool isScopeEnabledByDefault( const QString& name )
{
    if( name == "mono" ) return true;
    return false;
}

bool isScopeEnabled( const QSettings& settings, const QString& name )
{
    QString lname = name.toLower();
    return settings.value("prefs/scope/show-"+lname, isScopeEnabledByDefault(lname) ).toBool();
}

void MainWindow::honourPrefs() {
    update_check_updates();
    updateLogAutoScroll();
    changeGUITransparency(piSettings->gui_transparency);
    toggleScopeAxes();
    toggleMidi(1);
    toggleOSCServer(1);
    toggleIcons();
    scope();
}

void MainWindow::setMessageBoxStyle() {
    // Set text color to black and background colors to white for the error message display
    QPalette p = QApplication::palette();
    p.setColor(QPalette::WindowText,"#000");
    p.setColor(QPalette::ButtonText,"#000");
    p.setColor(QPalette::Text,"#000");
    p.setColor(QPalette::Base,"#FFF");
    QApplication::setPalette(p);
}

void MainWindow::invokeStartupError(QString msg) {
    if(startup_error_reported->isChecked()) {
        return;
    }

    startup_error_reported->setChecked(true);
    sonicPiOSCServer->stop();
    QMetaObject::invokeMethod(this, "startupError",
            Qt::QueuedConnection,
            Q_ARG(QString, msg));
}

void MainWindow::startupError(QString msg) {
    splashClose();
    setMessageBoxStyle();
    QString gui_log;
    QString scsynth_log;
    QString processes_log;
    QString server_output_log;
    QString server_error_log;
    if(homeDirWritable) {
        gui_log = readFile(gui_log_path);
        scsynth_log = readFile(scsynth_log_path);
        processes_log = readFile(process_log_path);
        server_output_log = readFile(server_output_log_path);
        server_error_log = readFile(server_error_log_path);
    }
    else {
        gui_log = "Permissions error: unable to access log";
        scsynth_log = "Permissions error: unable to access log";
        server_output_log = "Permissions error: unable to access log";
        server_error_log = "Permissions error: unable to access log";
        processes_log = "Permissions error: unable to access log";
    }

    QMessageBox *box = new QMessageBox(QMessageBox::Warning,
            tr("Server boot error..."), tr("Sonic Pi Boot Error\n\nApologies, a critical error occurred during startup") + ":\n\n " + msg + "\n\n" + tr("Please consider reporting a bug at") + "\nhttp://github.com/samaaron/sonic-pi/issues");
    QString error_report = "Sonic Pi Boot Error Report\n==================\n\n\nSystem Information\n----------------\n\n* Sonic Pi version: " + version + "\n* OS: " + osDescription() + "\n\n\nGUI Log\n-------\n\n**`" + gui_log_path + "`**\n```\n" + gui_log + "\n```\n\n\nServer Errors\n-------------\n\n**`" + server_error_log_path + "`**\n```\n" + server_error_log + "\n```\n\n\nServer Output\n-------------\n\n**`" + server_output_log_path + "`**\n```\n" + server_output_log + "\n```\n\n\nScsynth Output\n--------------\n\n**`" + scsynth_log_path + "`**\n```\n" + scsynth_log + "\n```\n\n\nProcess Log\n--------------\n\n**`" + process_log_path + "`**\n```\n" + processes_log + "\n\n\n```\n";
    box->setDetailedText(error_report);

    QGridLayout* layout = (QGridLayout*)box->layout();
    QSpacerItem* hSpacer = new QSpacerItem(200, 0, QSizePolicy::Minimum, QSizePolicy::Expanding);
    layout->addItem(hSpacer, layout->rowCount(), 0, 1, layout->columnCount());
    box->exec();
    std::cout << "[GUI] - Aborting. Sorry about this." << std::endl;
    QApplication::exit(-1);
    exit(EXIT_FAILURE);
}

void MainWindow::replaceBuffer(QString id, QString content, int line, int index, int first_line) {
    SonicPiScintilla* ws = filenameToWorkspace(id.toStdString());
    ws->replaceBuffer(content, line, index, first_line);
}

void MainWindow::replaceBufferIdx(int buf_idx, QString content, int line, int index, int first_line) {
    //  statusBar()->showMessage(tr("Replacing Buffer..."), 1000);
    SonicPiScintilla* ws = workspaces[buf_idx];
    ws->replaceBuffer(content, line, index, first_line);
}

void MainWindow::replaceLines(QString id, QString content, int start_line, int finish_line, int point_line, int point_index) {
    SonicPiScintilla* ws = filenameToWorkspace(id.toStdString());
    ws->replaceLines(start_line, finish_line, content);
    ws->setCursorPosition(point_line, point_index);
}

QString MainWindow::osDescription() {
#if QT_VERSION >= 0x050400
    return QSysInfo::prettyProductName();
#else
    // prettyProductName requires QT 5.4
    //
    return QString("Unknown OS");
#endif
}

std::string MainWindow::number_name(int i) {
    switch(i) {
        case 0: return "zero";
        case 1: return "one";
        case 2: return "two";
        case 3: return "three";
        case 4: return "four";
        case 5: return "five";
        case 6: return "six";
        case 7: return "seven";
        case 8: return "eight";
        case 9: return "nine";
        default: assert(false); return "";
    }
}

void MainWindow::loadWorkspaces()
{
    std::cout << "[GUI] - loading workspaces" << std::endl;

    for(int i = 0; i < workspace_max; i++) {
        Message msg("/load-buffer");
        msg.pushStr(guiID.toStdString());
        std::string s = "workspace_" + number_name(i);
        msg.pushStr(s);
        sendOSC(msg);
    }
}

void MainWindow::saveWorkspaces()
{
    std::cout << "[GUI] - saving workspaces" << std::endl;

    for(int i = 0; i < workspace_max; i++) {
        std::string code = workspaces[i]->text().toStdString();
        Message msg("/save-buffer");
        msg.pushStr(guiID.toStdString());
        std::string s = "workspace_" + number_name(i);
        msg.pushStr(s);
        msg.pushStr(code);
        sendOSC(msg);
    }
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    writeSettings();
    std::cout.rdbuf(coutbuf); // reset to stdout before exiting
    event->accept();
}

QString MainWindow::currentTabLabel()
{
    return tabs->tabText(tabs->currentIndex());
}


bool MainWindow::loadFile()
{
    QString selfilter = QString("%1 (*.rb *.txt)").arg(tr("Buffer files"));
    QSettings settings("sonic-pi.net", "gui-settings");
    QString lastDir = settings.value("lastDir", QDir::homePath() + "/Desktop").toString();
    QString fileName = QFileDialog::getOpenFileName(this, tr("Load Sonic Pi Buffer"), lastDir, QString("%1 (*.rb *.txt);;%2 (*.txt);;%3 (*.rb);;%4 (*.*)").arg(tr("Buffer files")).arg(tr("Text files")).arg(tr("Ruby files")).arg(tr("All files")), &selfilter) ;
    if(!fileName.isEmpty()){
        QFileInfo fi=fileName;
        settings.setValue("lastDir", fi.dir().absolutePath());
        SonicPiScintilla* p = (SonicPiScintilla*)tabs->currentWidget();
        loadFile(fileName, p);
        return true;
    } else {
        return false;
    }
}

bool MainWindow::saveAs()
{
    QString selfilter = QString("%1 (*.rb *.txt)").arg(tr("Buffer files"));
    QSettings settings("sonic-pi.net", "gui-settings");
    QString lastDir = settings.value("lastDir", QDir::homePath() + "/Desktop").toString();
    QString fileName = QFileDialog::getSaveFileName(this, tr("Save Current Buffer"), lastDir, QString("%1 (*.rb *.txt);;%2 (*.txt);;%3 (*.rb);;%4 (*.*)").arg(tr("Buffer files")).arg(tr("Text files")).arg(tr("Ruby files")).arg(tr("All files")), &selfilter) ;

    if(!fileName.isEmpty()){
        QFileInfo fi=fileName;
        settings.setValue("lastDir", fi.dir().absolutePath());
        if (!fileName.contains(QRegExp("\\.[a-z]+$"))) {
            fileName = fileName + ".txt";
        }
        return saveFile(fileName, (SonicPiScintilla*)tabs->currentWidget());
    } else {
        return false;
    }
}


void MainWindow::resetErrorPane() {
    errorPane->clear();
    errorPane->hide();
}

void MainWindow::runBufferIdx(int idx)
{
    QMetaObject::invokeMethod(tabs, "setCurrentIndex", Q_ARG(int, idx));
    runCode();
}

void MainWindow::showError(QString msg) {
    QString style_sheet = "qrc:///html/styles.css";
    if(piSettings->theme == SonicPiTheme::DarkMode || piSettings->theme == SonicPiTheme::DarkProMode) {
        style_sheet = "qrc:///html/dark_styles.css";
    }
    errorPane->clear();
    errorPane->setHtml("<html><head><link rel=\"stylesheet\" type=\"text/css\" href=\"" + style_sheet + "\"/></head><body>"  + msg + "</body></html>");
    errorPane->show();
}

void MainWindow::showBufferCapacityError() {
    showError("<h2 class=\"syntax_error_description\"><pre>GUI Error: Buffer Full</pre></h2><pre class=\"error_msg\"> Your code buffer has reached capacity. <br/> Please remove some code before continuing. <br/><span class=\"error_line\"> For working with very large buffers use: <br/> run_file \"/path/to/buffer.rb\"</span></pre>");
}

void MainWindow::runCode()
{
    scopeInterface->resume();

    // move log cursors to end of log files
    // and disable user input
    incomingPane->setTextInteractionFlags(Qt::NoTextInteraction);
    QTextCursor newIncomingCursor = incomingPane->textCursor();
    newIncomingCursor.movePosition(QTextCursor::End);
    incomingPane->setTextCursor(newIncomingCursor);

    outputPane->setTextInteractionFlags(Qt::NoTextInteraction);
    QTextCursor newOutputCursor = outputPane->textCursor();
    newOutputCursor.movePosition(QTextCursor::End);
    outputPane->setTextCursor(newOutputCursor);

    update();
    SonicPiScintilla *ws = (SonicPiScintilla*)tabs->currentWidget();

    QString code = ws->text();

    if(!piSettings->print_output) {
        code = "use_debug false #__nosave__ set by Qt GUI user preferences.\n" + code ;
    }

    if(!piSettings->log_cues) {
        code = "use_cue_logging false #__nosave__ set by Qt GUI user preferences.\n" + code ;
    }

    if(piSettings->check_args) {
        code = "use_arg_checks true #__nosave__ set by Qt GUI user preferences.\n" + code ;
    }

    if(piSettings->enable_external_synths) {
        code = "use_external_synths true #__nosave__ set by Qt GUI user preferences.\n" + code ;
    }

    if(piSettings->synth_trigger_timing_guarantees) {
        code = "use_timing_guarantees true #__nosave__ set by Qt GUI user preferences.\n" + code ;
    }

    code = "use_midi_defaults channel: \"" + piSettings->midi_default_channel_str+ "\" #__nosave__ set by Qt GUI user preferences.\n" + code ;

    if(piSettings->auto_indent_on_run) {
        beautifyCode();
    }

    ws->highlightCurrentLine();
    lexer->highlightAll();
    QTimer::singleShot(500, lexer, SLOT(unhighlightAll()));
    QTimer::singleShot(500, ws, SLOT(unhighlightCurrentLine()));
    ws->clearLineMarkers();
    resetErrorPane();

    //std::string code = ws->text().toStdString();
    Message msg("/save-and-run-buffer");
    msg.pushStr(guiID.toStdString());

    std::string filename = ((SonicPiScintilla*)tabs->currentWidget())->fileName.toStdString();
    msg.pushStr(filename);

    if(piSettings->clear_output_on_run){
        outputPane->clear();
    }

    msg.pushStr(code.toStdString());
    msg.pushStr(filename);
    bool res = sendOSC(msg);

    if(!res){
        showBufferCapacityError();
        return;
    }

    statusBar()->showMessage(tr("Running Code..."), 1000);

}

void MainWindow::zoomCurrentWorkspaceIn()
{
    statusBar()->showMessage(tr("Zooming In..."), 2000);
    SonicPiScintilla* ws = ((SonicPiScintilla*)tabs->currentWidget());
    ws->zoomFontIn();
}

void MainWindow::zoomCurrentWorkspaceOut()
{
    statusBar()->showMessage(tr("Zooming Out..."), 2000);
    SonicPiScintilla* ws = ((SonicPiScintilla*)tabs->currentWidget());
    ws->zoomFontOut();
}

void MainWindow::beautifyCode()
{
    statusBar()->showMessage(tr("Beautifying..."), 2000);
    SonicPiScintilla* ws = ((SonicPiScintilla*)tabs->currentWidget());
    std::string code = ws->text().toStdString();
    int line = 0;
    int index = 0;
    ws->getCursorPosition(&line, &index);
    int first_line = ws->firstVisibleLine();
    Message msg("/buffer-beautify");
    msg.pushStr(guiID.toStdString());
    std::string filename = ((SonicPiScintilla*)tabs->currentWidget())->fileName.toStdString();
    msg.pushStr(filename);
    msg.pushStr(code);
    msg.pushInt32(line);
    msg.pushInt32(index);
    msg.pushInt32(first_line);
    sendOSC(msg);
}


bool MainWindow::sendOSC(Message m)
{
    bool res = oscSender->sendOSC(m);
    if(!res) {
        std::cout << "[GUI] - Could Not Send OSC" << std::endl;
    }
    return res;
}

void MainWindow::reloadServerCode()
{
    statusBar()->showMessage(tr("Reloading..."), 2000);
    Message msg("/reload");
    msg.pushStr(guiID.toStdString());
    sendOSC(msg);
}

void MainWindow::check_for_updates_now() {
    statusBar()->showMessage(tr("Checking for updates..."), 2000);
    Message msg("/check-for-updates-now");
    msg.pushStr(guiID.toStdString());
    sendOSC(msg);
}

void MainWindow::enableCheckUpdates()
{
    statusBar()->showMessage(tr("Enabling update checking..."), 2000);
    Message msg("/enable-update-checking");
    msg.pushStr(guiID.toStdString());
    sendOSC(msg);
}

void MainWindow::disableCheckUpdates()
{
    statusBar()->showMessage(tr("Disabling update checking..."), 2000);
    Message msg("/disable-update-checking");
    msg.pushStr(guiID.toStdString());
    sendOSC(msg);
}

void MainWindow::mixerHpfEnable(float freq)
{
    statusBar()->showMessage(tr("Enabling Mixer HPF..."), 2000);
    Message msg("/mixer-hpf-enable");
    msg.pushStr(guiID.toStdString());
    msg.pushFloat(freq);
    sendOSC(msg);
}

void MainWindow::mixerHpfDisable()
{
    statusBar()->showMessage(tr("Disabling Mixer HPF..."), 2000);
    Message msg("/mixer-hpf-disable");
    msg.pushStr(guiID.toStdString());
    sendOSC(msg);
}

void MainWindow::mixerLpfEnable(float freq)
{
    statusBar()->showMessage(tr("Enabling Mixer LPF..."), 2000);
    Message msg("/mixer-lpf-enable");
    msg.pushStr(guiID.toStdString());
    msg.pushFloat(freq);
    sendOSC(msg);
}

void MainWindow::mixerLpfDisable()
{
    statusBar()->showMessage(tr("Disabling Mixer LPF..."), 2000);
    Message msg("/mixer-lpf-disable");
    msg.pushStr(guiID.toStdString());
    sendOSC(msg);
}

void MainWindow::mixerInvertStereo()
{
    statusBar()->showMessage(tr("Enabling Inverted Stereo..."), 2000);
    Message msg("/mixer-invert-stereo");
    msg.pushStr(guiID.toStdString());
    sendOSC(msg);
}

void MainWindow::mixerStandardStereo()
{
    statusBar()->showMessage(tr("Enabling Standard Stereo..."), 2000);
    Message msg("/mixer-standard-stereo");
    msg.pushStr(guiID.toStdString());
    sendOSC(msg);
}

void MainWindow::mixerMonoMode()
{
    statusBar()->showMessage(tr("Mono Mode..."), 2000);
    Message msg("/mixer-mono-mode");
    msg.pushStr(guiID.toStdString());
    sendOSC(msg);
}

void MainWindow::mixerStereoMode()
{
    statusBar()->showMessage(tr("Stereo Mode..."), 2000);
    Message msg("/mixer-stereo-mode");
    msg.pushStr(guiID.toStdString());
    sendOSC(msg);
}

void MainWindow::stopCode()
{
    stopRunningSynths();
    statusBar()->showMessage(tr("Stopping..."), 2000);
}

void MainWindow::scopeVisibilityChanged() {
    piSettings->show_scopes = scopeWidget->isVisible();
    scopeAct->setIcon( theme->getScopeIcon(piSettings->show_scopes));
    emit settingsChanged();
}

void MainWindow::toggleScope() {
    piSettings->show_scopes = !piSettings->show_scopes;
    emit settingsChanged();
    scope();
}

void MainWindow::scope() {
    scopeAct->setIcon( theme->getScopeIcon(piSettings->show_scopes));
    if(piSettings->show_scopes) {
        for( auto name : scopeInterface->getScopeNames()) {
            scopeInterface->enableScope( name, piSettings->isScopeActive(name));
        }
        scopeWidget->show();

    } else {
        scopeWidget->hide();
    }
}

void MainWindow::about() {
    // todo: this is returning true even after the window disappears
    // Qt::Tool windows get closed automatically when app loses focus
    if(infoWidg->isVisible()) {
        infoWidg->hide();

    } else {
        infoWidg->raise();
        infoWidg->show();
    }
    infoAct->setIcon( theme->getInfoIcon( infoWidg->isVisible()));
}

void MainWindow::toggleHelpIcon() {
    helpAct->setIcon( theme->getHelpIcon( docWidget->isVisible()));
}
void MainWindow::help() {
    statusBar()->showMessage(tr("help visibility changed..."), 2000);
    if(docWidget->isVisible()) {
        docWidget->hide();
    } else {
        docWidget->show();
    }
    helpAct->setIcon( theme->getHelpIcon( docWidget->isVisible()));
}

void MainWindow::helpContext() {
    if (!docWidget->isVisible())
        docWidget->show();
    SonicPiScintilla *ws = ((SonicPiScintilla*)tabs->currentWidget());
    QString selection = ws->selectedText();
    if (selection == "") { // get current word instead
        int line, pos;
        ws->getCursorPosition(&line, &pos);
        QString text = ws->text(line);
        selection = ws->wordAtLineIndex(line, pos);
    }
    selection = selection.toLower();
    if (selection[0] == ':')
        selection = selection.mid(1);

    if (helpKeywords.contains(selection)) {
        struct help_entry entry = helpKeywords[selection];
        QListWidget *list = helpLists[entry.pageIndex];

        // force current row to be changed
        // by setting it to a different value to
        // entry.entryIndex and then setting it
        // back. That way it always gets displayed
        // in the GUI :-)
        if (entry.entryIndex == 0) {
            list->setCurrentRow(1);
        } else {
            list->setCurrentRow(0);
        }
        docsCentral->setCurrentIndex(entry.pageIndex);
        list->setCurrentRow(entry.entryIndex);
    }
}

void MainWindow::changeGUITransparency(int val) {
    // scale it linearly from 0 -> 100 to 0.3 -> 1
    setWindowOpacity((0.7 * ((100 - (float)val) / 100.0))  + 0.3);
}

void MainWindow::changeSystemPreAmp(int val, int silent)
{
    std::cout << "[GUI] Change Volume to " << val << std::endl;
    float v = (float) val;
    v = (v / 100.0) * 2.0;
    Message msg("/mixer-amp");
    msg.pushStr(guiID.toStdString());
    msg.pushFloat(v);
    msg.pushInt32(silent);
    sendOSC(msg);
    statusBar()->showMessage(tr("Updating System Volume..."), 2000);
}

void MainWindow::toggleScope(QString name) {
    scopeInterface->enableScope( name, piSettings->isScopeActive(name));
}

void MainWindow::toggleLeftScope()
{
    //scopeInterface->enableScope("Left",show_left_scope->isChecked());
}

void MainWindow::toggleRightScope()
{
    //scopeInterface->enableScope("Right",show_right_scope->isChecked());
}

void MainWindow::toggleScopeAxes()
{
    scopeInterface->setScopeAxes(piSettings->show_scope_axes);
}

void MainWindow::cycleThemes() {
    if ( piSettings->theme == SonicPiTheme::LightMode ) { 
        piSettings->theme = SonicPiTheme::DarkMode;
    } else if ( piSettings->theme == SonicPiTheme::DarkMode ) { 
        piSettings->theme = SonicPiTheme::LightProMode;
    } else if ( piSettings->theme == SonicPiTheme::LightProMode ) { 
        piSettings->theme = SonicPiTheme::DarkProMode;
    } else if ( piSettings->theme == SonicPiTheme::DarkProMode ) { 
        piSettings->theme = SonicPiTheme::HighContrastMode;
    } else if ( piSettings->theme == SonicPiTheme::HighContrastMode ) { 
        piSettings->theme = SonicPiTheme::LightMode;
    }
    updateColourTheme();
}

void MainWindow::updateLogAutoScroll() {
    bool val = piSettings->log_auto_scroll;
    outputPane->forceScrollDown(val);
    if(val) {
        statusBar()->showMessage(tr("Log Auto Scroll on..."), 2000);
    } else {
        statusBar()->showMessage(tr("Log Auto Scroll off..."), 2000);
    }
}

void MainWindow::toggleIcons() {
    runAct->setIcon(theme->getRunIcon());
    stopAct->setIcon(theme->getStopIcon());
    saveAsAct->setIcon(theme->getSaveAsIcon());
    loadFileAct->setIcon(theme->getLoadIcon());
    textIncAct->setIcon(theme->getTextIncIcon());
    textDecAct->setIcon(theme->getTextDecIcon());

    helpAct->setIcon(theme->getHelpIcon(docWidget->isVisible()));
    recAct->setIcon(theme->getRecIcon(false, false));
    prefsAct->setIcon(theme->getPrefsIcon(prefsWidget->isVisible()));
    infoAct->setIcon(theme->getInfoIcon(infoWidg->isVisible()));
    scopeAct->setIcon(theme->getScopeIcon(scopeWidget->isVisible()));

    if (piSettings->theme == SonicPiTheme::DarkProMode ||
        piSettings->theme == SonicPiTheme::LightProMode) {
        toolBar->setIconSize(QSize(30, 30));
    } else {
        toolBar->setIconSize(QSize(84.6, 30.0));
    }
}

void MainWindow::updateColourTheme(){
    theme->switchTheme( piSettings->theme );
    statusBar()->showMessage(tr("Colour Theme: ")+theme->getName(), 2000);

    QString css = theme->getCss();
    toggleIcons();

    docPane->document()->setDefaultStyleSheet(css);
    docPane->reload();

    foreach(QTextBrowser* pane, infoPanes) {
        pane->document()->setDefaultStyleSheet(css);
        pane->reload();
    }

    errorPane->document()->setDefaultStyleSheet(css);

    // clear stylesheets
    this->setStyleSheet("");
    infoWidg->setStyleSheet("");
    mainWidget->setStyleSheet("");
    statusBar()->setStyleSheet("");
    outputPane->setStyleSheet("");
    outputWidget->setStyleSheet("");
    prefsWidget->setStyleSheet("");
    tabs->setStyleSheet("");
    //TODO inject to settings Widget
    //prefTabs->setStyleSheet("");
    docsCentral->setStyleSheet("");
    docWidget->setStyleSheet("");
    toolBar->setStyleSheet("");
    scopeWidget->setStyleSheet("");

    QPalette p = theme->createPalette();
    QApplication::setPalette(p);

    QString appStyling = theme->getAppStylesheet();

    this->setStyleSheet(appStyling);
    infoWidg->setStyleSheet(appStyling);

    errorPane->setStyleSheet(theme->getErrorStylesheet());
    docsCentral->setStyleSheet(theme->getDocStylesheet());

    scopeInterface->refresh();
    scopeWidget->update();

    for(int i=0; i < tabs->count(); i++){
        SonicPiScintilla *ws = (SonicPiScintilla *)tabs->widget(i);
        ws->setFrameShape(QFrame::NoFrame);
        ws->setStyleSheet(appStyling);

        if (piSettings->theme == SonicPiTheme::HighContrastMode) {
            ws->setCaretWidth(8);
        } else {
            ws->setCaretWidth(5);
        }
        ws->redraw();
    }

    scopeInterface->setColor(theme->color("Scope"));
    lexer->unhighlightAll();
}

void MainWindow::changeShowLineNumbers(){
    bool show = piSettings->show_line_numbers;
    for(int i=0; i < tabs->count(); i++){
        SonicPiScintilla *ws = (SonicPiScintilla *)tabs->widget(i);
        if (show) {
            ws->showLineNumbers();
        } else {
            ws->hideLineNumbers();
        }
    }
}

void MainWindow::togglePrefs() {
    if(prefsWidget->isVisible()) {
        prefsWidget->hide();
    } else {
        prefsWidget->show();
    }
    updatePrefsIcon();
}

void MainWindow::updatePrefsIcon()
{
    prefsAct->setIcon(theme->getPrefsIcon(prefsWidget->isVisible()));
}


void MainWindow::wheelEvent(QWheelEvent *event)
{
#if defined(Q_OS_WIN)
    if (event->modifiers() & Qt::ControlModifier) {
        SonicPiScintilla* ws = ((SonicPiScintilla*)tabs->currentWidget());
        if (event->angleDelta().y() > 0)
            ws->zoomFontIn();
        else
            ws->zoomFontOut();
    }
#else
    (void)event;
#endif
}

void MainWindow::stopRunningSynths()
{
    Message msg("/stop-all-jobs");
    msg.pushStr(guiID.toStdString());
    sendOSC(msg);
}

void MainWindow::clearOutputPanels()
{
    outputPane->clear();
    errorPane->clear();
}

QKeySequence MainWindow::ctrlKey(char key)
{
#ifdef Q_OS_MAC
    return QKeySequence(QString("Meta+%1").arg(key));
#else
    return QKeySequence(QString("Ctrl+%1").arg(key));
#endif
}

// Cmd on Mac, Alt everywhere else
QKeySequence MainWindow::metaKey(char key)
{
#ifdef Q_OS_MAC
    return QKeySequence(QString("Ctrl+%1").arg(key));
#else
    return QKeySequence(QString("alt+%1").arg(key));
#endif
}

Qt::Modifier MainWindow::metaKeyModifier()
{
#ifdef Q_OS_MAC
    return Qt::CTRL;
#else
    return Qt::ALT;
#endif
}

QKeySequence MainWindow::shiftMetaKey(char key)
{
#ifdef Q_OS_MAC
    return QKeySequence(QString("Shift+Ctrl+%1").arg(key));
#else
    return QKeySequence(QString("Shift+alt+%1").arg(key));
#endif
}

QKeySequence MainWindow::ctrlMetaKey(char key)
{
#ifdef Q_OS_MAC
    return QKeySequence(QString("Ctrl+Meta+%1").arg(key));
#else
    return QKeySequence(QString("Ctrl+alt+%1").arg(key));
#endif
}

QKeySequence MainWindow::ctrlShiftMetaKey(char key)
{
#ifdef Q_OS_MAC
    return QKeySequence(QString("Shift+Ctrl+Meta+%1").arg(key));
#else
    return QKeySequence(QString("Shift+Ctrl+alt+%1").arg(key));
#endif
}

char MainWindow::int2char(int i){
    return '0' + i;
}

QString MainWindow::tooltipStrShiftMeta(char key, QString str) {
#ifdef Q_OS_MAC
    return QString("%1 (⇧⌘%2)").arg(str).arg(key);
#else
    return QString("%1 (Shift-alt-%2)").arg(str).arg(key);
#endif
}

QString MainWindow::tooltipStrMeta(char key, QString str) {
#ifdef Q_OS_MAC
    return QString("%1 (⌘%2)").arg(str).arg(key);
#else
    return QString("%1 (alt-%2)").arg(str).arg(key);
#endif
}

void MainWindow::updateAction(QAction *action, QShortcut *sc, QString tooltip,  QString desc = "")
{
    QString shortcutDesc = sc->key().toString(QKeySequence::NativeText);
    action->setToolTip(tooltip + " (" + shortcutDesc + ")");
    if (desc == "")
    {
        action->setText(action->iconText() + "\t" + shortcutDesc);
    } else {
        action->setText(desc + "\t" + shortcutDesc);
    }
    action->setStatusTip(tooltip + " (" + shortcutDesc + ")");
}

void MainWindow::createShortcuts()
{
    std::cout << "[GUI] - creating shortcuts" << std::endl;
    new QShortcut(shiftMetaKey('['), this, SLOT(tabPrev()));
    new QShortcut(shiftMetaKey(']'), this, SLOT(tabNext()));
    new QShortcut(QKeySequence("F8"), this, SLOT(reloadServerCode()));
    new QShortcut(QKeySequence("F9"), this, SLOT(toggleButtonVisibility()));
    new QShortcut(shiftMetaKey('B'), this, SLOT(toggleButtonVisibility()));
    new QShortcut(QKeySequence("F10"), this, SLOT(toggleFocusMode()));
    new QShortcut(shiftMetaKey('F'), this, SLOT(toggleFullScreenMode()));
    new QShortcut(shiftMetaKey('M'), this, SLOT(cycleThemes()));
    new QShortcut(QKeySequence("F11"), this, SLOT(toggleLogVisibility()));
    new QShortcut(shiftMetaKey('L'), this, SLOT(toggleLogVisibility()));
    new QShortcut(QKeySequence("F12"),this, SLOT(toggleScopePaused()));
}

void  MainWindow::createToolBar()
{
    std::cout << "[GUI] - creating tool bar" << std::endl;
    // Run
    runAct = new QAction(theme->getRunIcon(), tr("Run"), this);
    runSc = new QShortcut(metaKey('R'), this, SLOT(runCode()));
    updateAction(runAct, runSc, tr("Run the code in the current buffer"));
    connect(runAct, SIGNAL(triggered()), this, SLOT(runCode()));

    // Stop
    stopAct = new QAction(theme->getStopIcon(), tr("Stop"), this);
    stopSc = new QShortcut(metaKey('S'), this, SLOT(stopCode()));
    updateAction(stopAct, stopSc, tr("Stop all running code"));
    connect(stopAct, SIGNAL(triggered()), this, SLOT(stopCode()));

    // Record
    recAct = new QAction(theme->getRecIcon(false, false), tr("Start Recording"), this);
    recSc = new QShortcut(shiftMetaKey('R'), this, SLOT(toggleRecording()));
    updateAction(recAct, recSc, tr("Start recording to a WAV audio file"));
    connect(recAct, SIGNAL(triggered()), this, SLOT(toggleRecording()));

    // Save
    saveAsAct = new QAction(theme->getSaveAsIcon(), tr("Save"), this);
    saveAsSc = new QShortcut(shiftMetaKey('S'), this, SLOT(saveAs()));
    updateAction(saveAsAct, saveAsSc, tr("Save current buffer as an external file"));
    connect(saveAsAct, SIGNAL(triggered()), this, SLOT(saveAs()));

    // Load
    loadFileAct = new QAction(theme->getLoadIcon(), tr("Load"), this);
    loadFileSc = new QShortcut(shiftMetaKey('O'), this, SLOT(loadFile()));
    updateAction(loadFileAct, loadFileSc,  tr("Load an external file in the current buffer"));
    connect(loadFileAct, SIGNAL(triggered()), this, SLOT(loadFile()));

    // Align
    textAlignAct = new QAction(QIcon(":/images/align.png"),
            tr("Auto-Align Text"), this);
    textAlignSc = new QShortcut(metaKey('M'), this, SLOT(beautifyCode()));
    updateAction(textAlignAct, textAlignSc, tr("Align code to improve readability"));
    connect(textAlignAct, SIGNAL(triggered()), this, SLOT(beautifyCode()));

    // Font Size Increase
    textIncAct = new QAction(theme->getTextIncIcon(), tr("Text Size Up"), this);
    textIncSc = new QShortcut(metaKey('+'), this, SLOT(zoomCurrentWorkspaceIn()));
    updateAction(textIncAct, textIncSc, tr("Increase Text Size"));
    connect(textIncAct, SIGNAL(triggered()), this, SLOT(zoomCurrentWorkspaceIn()));

    // Font Size Decrease
    textDecAct = new QAction(theme->getTextDecIcon(), tr("Text Size Down"), this);
    textDecSc = new QShortcut(metaKey('-'), this, SLOT(zoomCurrentWorkspaceOut()));
    updateAction(textDecAct, textDecSc, tr("Decrease Text Size"));
    connect(textDecAct, SIGNAL(triggered()), this, SLOT(zoomCurrentWorkspaceOut()));

    // Scope
    scopeAct = new QAction(theme->getScopeIcon(false), tr("Toggle Scope"), this);
    scopeSc = new QShortcut(metaKey('O'), this, SLOT(toggleScope()));
    updateAction(scopeAct, scopeSc, tr("Toggle visibility of audio oscilloscope"));
    connect(scopeAct, SIGNAL(triggered()), this, SLOT(toggleScope()));

    // Info
    infoAct = new QAction(theme->getInfoIcon(false), tr("Show Info"), this);
    infoSc = new QShortcut(metaKey('1'), this, SLOT(about()));
    updateAction(infoAct, infoSc, tr("See information about Sonic Pi"));
    connect(infoAct, SIGNAL(triggered()), this, SLOT(about()));

    // Help
    helpAct = new QAction(theme->getHelpIcon(false), tr("Toggle Help"), this);
    helpSc = new QShortcut(metaKey('I'), this, SLOT(help()));
    updateAction(helpAct, helpSc, tr("Toggle the visibility of the help pane"));
    connect(helpAct, SIGNAL(triggered()), this, SLOT(help()));

    // Preferences
    prefsAct = new QAction(theme->getPrefsIcon(false), tr("Toggle Preferences"), this);
    prefsSc = new QShortcut(metaKey('P'), this, SLOT(togglePrefs()));
    updateAction(prefsAct, prefsSc, tr("Toggle the visibility of the preferences pane"));
    connect(prefsAct, SIGNAL(triggered()), this, SLOT(togglePrefs()));

    QWidget *spacer = new QWidget();
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    toolBar = addToolBar(tr("Tools"));
    toolBar->setObjectName("toolbar");

    toolBar->addAction(runAct);
    toolBar->addAction(stopAct);
    toolBar->addAction(recAct);

    toolBar->addAction(saveAsAct);
    toolBar->addAction(loadFileAct);

    toolBar->addWidget(spacer);

    toolBar->addAction(textDecAct);
    toolBar->addAction(textIncAct);

    dynamic_cast<QToolButton*>(toolBar->widgetForAction(textDecAct))->setAutoRepeat(true);
    dynamic_cast<QToolButton*>(toolBar->widgetForAction(textIncAct))->setAutoRepeat(true);

    toolBar->addAction(scopeAct);
    toolBar->addAction(infoAct);
    toolBar->addAction(helpAct);
    toolBar->addAction(prefsAct);

    fileMenu = menuBar()->addMenu(tr("&File"));
    fileMenu->addAction(runAct);
    fileMenu->addAction(stopAct);
    fileMenu->addAction(recAct);
    fileMenu->addAction(saveAsAct);
    fileMenu->addAction(loadFileAct);

    editMenu = menuBar()->addMenu(tr("&Edit"));
    editMenu->addAction(textIncAct);
    editMenu->addAction(textDecAct);

    windowMenu = menuBar()->addMenu(tr("&Window"));
    windowMenu->addAction(scopeAct);
    windowMenu->addAction(infoAct);
    windowMenu->addAction(helpAct);
    windowMenu->addAction(prefsAct);

}

QString MainWindow::readFile(QString name)
{
    QFile file(name);
    if (!file.open(QFile::ReadOnly | QFile::Text)) {
        std::cerr << "[GUI] - could not open file " << name.toStdString() << "\n";
        return "";
    }

    QTextStream st(&file);
    st.setCodec("UTF-8");
    return st.readAll();
}

void MainWindow::createInfoPane() {
    std::cout << "[GUI] - creating info panel" << std::endl;
    QTabWidget* infoTabs = new QTabWidget(this);

    QStringList urls, tabs;

    urls << "qrc:///html/info.html"
        << "qrc:///info/COMMUNITY.html"
        << "qrc:///info/CORETEAM.html"
        << "qrc:///info/CONTRIBUTORS.html"
        << "qrc:///info/LICENSE.html"
        << "qrc:///info/CHANGELOG.html";

    tabs << tr("About")
        << tr("Community")
        << tr("Core Team")
        << tr("Contributors")
        << tr("License")
        << tr("History");

    for (int t=0; t < urls.size(); t++) {
        QTextBrowser *pane = new QTextBrowser;
        infoPanes.append(pane);
        addUniversalCopyShortcuts(pane);
        pane->setOpenExternalLinks(true);
        pane->setSource(QUrl(urls[t]));
        infoTabs->addTab(pane, tabs[t]);
    }

    infoTabs->setTabPosition(QTabWidget::South);

    QBoxLayout *infoLayout = new QBoxLayout(QBoxLayout::LeftToRight);
    infoLayout->addWidget(infoTabs);

    infoWidg = new InfoWidget;
    infoWidg->setWindowIcon(QIcon(":images/icon-smaller.png"));
    infoWidg->setLayout(infoLayout);
    infoWidg->setWindowFlags(Qt::Tool | Qt::WindowTitleHint | Qt::WindowCloseButtonHint | Qt::CustomizeWindowHint | Qt::WindowStaysOnTopHint);
    infoWidg->setWindowTitle(tr("Sonic Pi - Info"));
    infoWidg->setFixedSize(660, 640);

    connect(infoWidg, SIGNAL(closed()), this, SLOT(about()));

    QAction *closeInfoAct = new QAction(this);
    closeInfoAct->setShortcut(QKeySequence(Qt::CTRL + Qt::Key_W));
    connect(closeInfoAct, SIGNAL(triggered()), this, SLOT(about()));
    infoWidg->addAction(closeInfoAct);
}


/**
 * Toggle record Icon while recording is active (triggert by rec_flash_timer)
 */
void MainWindow::toggleRecordingOnIcon() {
    show_rec_icon_a = !show_rec_icon_a;
    recAct->setIcon( theme->getRecIcon( true, show_rec_icon_a));
}

/**
 * Start or Stop recording
 */
void MainWindow::toggleRecording() {
    is_recording = !is_recording;
    if(is_recording) {
        updateAction(recAct, recSc, tr("Stop Recording"), tr("Stop Recording"));
        // recAct->setStatusTip(tr("Stop Recording"));
        // recAct->setToolTip(tr("Stop Recording"));
        // recAct->setText(tr("Stop Recording"));
        rec_flash_timer->start(500);
        Message msg("/start-recording");
        msg.pushStr(guiID.toStdString());
        sendOSC(msg);
    } else {
        rec_flash_timer->stop();
        updateAction(recAct, recSc, tr("Start Recording"), tr("Start Recording"));
        recAct->setIcon( theme->getRecIcon(is_recording, false));

        Message msg("/stop-recording");
        msg.pushStr(guiID.toStdString());
        sendOSC(msg);
        QSettings settings("sonic-pi.net", "gui-settings");
        QString lastDir = settings.value("lastDir", QDir::homePath() + "/Desktop").toString();
        QString fileName = QFileDialog::getSaveFileName(this, tr("Save Recording"), lastDir, tr("Wavefile (*.wav)"));
        if (!fileName.isEmpty()) {
            QFileInfo fi=fileName;
            settings.setValue("lastDir", fi.dir().absolutePath());
            Message msg("/save-recording");
            msg.pushStr(guiID.toStdString());
            msg.pushStr(fileName.toStdString());
            sendOSC(msg);
        } else {
            Message msg("/delete-recording");
            msg.pushStr(guiID.toStdString());
            sendOSC(msg);
        }
    }
}


void MainWindow::createStatusBar()
{
    std::cout << "[GUI] - creating status bar" << std::endl;
    versionLabel = new QLabel(this);
    versionLabel->setText("Sonic Pi");
    statusBar()->showMessage(tr("Ready..."));
    statusBar()->addPermanentWidget(versionLabel);
}

/**
 * restores the last size and position of the mainwindow
 * restores the zoomlevels of the editor tabs
 */
void MainWindow::restoreWindows() {
    QSettings settings("sonic-pi.net", "gui-settings");

    QPoint pos = settings.value("pos", QPoint(200, 200)).toPoint();
    QSize size = settings.value("size", QSize(400, 400)).toSize();

    int index = settings.value("workspace", 0).toInt();
    if (index < tabs->count())
        tabs->setCurrentIndex(index);

    for (int w=0; w < workspace_max; w++) {
        // default zoom is 13
        int zoom = settings.value(QString("workspace%1zoom").arg(w), 13)
            .toInt();
        if (zoom < -5) zoom = -5;
        if (zoom > 20) zoom = 20;

        workspaces[w]->setProperty("zoom", QVariant(zoom));
        workspaces[w]->zoomTo(zoom);
    }

    docsplit->restoreState(settings.value("docsplitState").toByteArray());
    bool visualizer = piSettings->show_scopes;
   restoreState(settings.value("windowState").toByteArray());
//    restoreGeometry(settings.value("windowGeom").toByteArray());

//    if (visualizer != piSettings->show_scopes) {
//        piSettings->show_scopes = visualizer;
//        scope();
//    }

    resize(size);
    move(pos);
}

/**
 * read the preferences
 *
 */
void MainWindow::readSettings() {
    std::cout << "[GUI] - reading settings" << std::endl;
    QSettings settings("sonic-pi.net", "gui-settings");

    piSettings->show_buttons = true;
    piSettings->show_tabs = true;
    piSettings->show_log = true;

    // Read in preferences from previous session
    piSettings->osc_public = settings.value("prefs/osc-public", false).toBool();
    piSettings->osc_server_enabled = settings.value("prefs/osc-enabled", true).toBool();
    piSettings->midi_enabled =  settings.value("prefs/midi-enable", true).toBool();
    piSettings->midi_default_channel =  settings.value("prefs/default-midi-channel", 0).toInt();
    piSettings->check_args =  settings.value("prefs/check-args", true).toBool();
    piSettings->print_output =  settings.value("prefs/print-output", true).toBool();
    piSettings->clear_output_on_run = settings.value("prefs/clear-output-on-run", true).toBool();
    piSettings->log_cues = settings.value("prefs/log-cues", false).toBool();
    piSettings->log_auto_scroll = settings.value("prefs/log-auto-scroll", true).toBool();
    piSettings->show_line_numbers =  settings.value("prefs/show-line-numbers", true).toBool();
    piSettings->enable_external_synths = settings.value("prefs/enable-external-synths", false).toBool();
    piSettings->synth_trigger_timing_guarantees = settings.value("prefs/synth-trigger-timing-guarantees", false).toBool();

    piSettings->main_volume = settings.value("prefs/system-vol", 80).toInt();
    piSettings->mixer_force_mono = settings.value("prefs/mixer-force-mono", false).toBool();
    piSettings->mixer_invert_stereo =  settings.value("prefs/mixer-invert-stereo", false).toBool();
    piSettings->check_updates = settings.value("prefs/rp/check-updates", true).toBool();
    piSettings->auto_indent_on_run = settings.value("prefs/auto-indent-on-run", true).toBool();
    piSettings->gui_transparency = settings.value("prefs/gui_transparency", 0).toInt();
    piSettings->show_scopes = settings.value("prefs/scope/show-scopes", true).toBool();
    piSettings->show_scope_axes = settings.value("prefs/scope/show-axes", false).toBool();
    piSettings->show_incoming_osc_log = settings.value("prefs/show_incoming_osc_log", true).toBool();

    emit settingsChanged();
}

void MainWindow::restoreScopeState(std::vector<QString> names) {
    std::cout << "[GUI] - restoring scope states " << std::endl;
    QSettings settings("sonic-pi.net", "gui-settings");

    for ( auto name : names ) {
        bool def = (name.toLower() == "mono"); 
        piSettings->setScopeState(name, settings.value("prefs/scope/show-"+name.toLower(), def).toBool());
    }
}

void MainWindow::writeSettings()
{
    std::cout << "[GUI] - writing settings" << std::endl;
    QSettings settings("sonic-pi.net", "gui-settings");
    settings.setValue("pos", pos());
    settings.setValue("size", size());
    settings.setValue("first_time", 0);

    settings.setValue("prefs/midi-default-channel", piSettings->midi_default_channel);
    settings.setValue("prefs/midi-enable", piSettings->midi_enabled);
    settings.setValue("prefs/osc-public",  piSettings->osc_public);
    settings.setValue("prefs/osc-enabled", piSettings->osc_server_enabled);

    settings.setValue("prefs/check-args", piSettings->check_args);
    settings.setValue("prefs/print-output", piSettings->print_output);
    settings.setValue("prefs/clear-output-on-run", piSettings->clear_output_on_run);
    settings.setValue("prefs/log-cues", piSettings->log_cues);
    settings.setValue("prefs/log-auto-scroll", piSettings->log_auto_scroll);
    settings.setValue("prefs/show-line-numbers", piSettings->show_line_numbers);
    settings.setValue("prefs/enable-external-synths", piSettings->enable_external_synths);
    settings.setValue("prefs/synth-trigger-timing-guarantees", piSettings->synth_trigger_timing_guarantees);
    settings.setValue("prefs/mixer-force-mono", piSettings->mixer_force_mono);
    settings.setValue("prefs/mixer-invert-stereo", piSettings->mixer_invert_stereo);
    settings.setValue("prefs/system-vol", piSettings->main_volume);
    settings.setValue("prefs/rp/check-updates", piSettings->check_updates);
    settings.setValue("prefs/auto-indent-on-run", piSettings->auto_indent_on_run);
    settings.setValue("prefs/gui_transparency", piSettings->gui_transparency);
    settings.setValue("prefs/scope/show-axes", piSettings->show_scope_axes );
    settings.setValue("prefs/scope/show-scopes", piSettings->show_scopes );
    settings.setValue("prefs/show_incoming_osc_log", piSettings->show_incoming_osc_log);

    for ( auto name : piSettings->scope_names ) {
        settings.setValue("prefs/scope/show-"+name.toLower(), piSettings->isScopeActive(name));
    }

    settings.setValue("workspace", tabs->currentIndex());

    for (int w=0; w < workspace_max; w++) {
        settings.setValue(QString("workspace%1zoom").arg(w),
                workspaces[w]->property("zoom"));
    }

    settings.setValue("docsplitState", docsplit->saveState());
    settings.setValue("windowState", saveState());
    settings.setValue("windowGeom", saveGeometry());
}

void MainWindow::loadFile(const QString &fileName, SonicPiScintilla* &text)
{
    QFile file(fileName);
    if (!file.open(QFile::ReadOnly)) {
        QMessageBox::warning(this, tr("Sonic Pi"),
                tr("Cannot read file %1:\n%2.")
                .arg(fileName)
                .arg(file.errorString()));
        updateColourTheme();
        return;
    }

    QTextStream in(&file);
    QApplication::setOverrideCursor(Qt::WaitCursor);
    text->setText(in.readAll());
    QApplication::restoreOverrideCursor();
    statusBar()->showMessage(tr("File loaded..."), 2000);
}

bool MainWindow::saveFile(const QString &fileName, SonicPiScintilla* text)
{
    QFile file(fileName);
    if (!file.open(QFile::WriteOnly)) {
        QMessageBox::warning(this, tr("Sonic Pi"),
                tr("Cannot write file %1:\n%2.")
                .arg(fileName)
                .arg(file.errorString()));
        updateColourTheme();
        return false;
    }

    QTextStream out(&file);

    QApplication::setOverrideCursor(Qt::WaitCursor);
    QString code = text->text();
#if defined(Q_OS_WIN)
    code.replace("\n", "\r\n"); // CRLF for Windows users
    code.replace("\r\r\n", "\r\n"); // don't double-replace if already encoded
#endif
    out << code;
    QApplication::restoreOverrideCursor();

    statusBar()->showMessage(tr("File saved..."), 2000);
    return true;
}

SonicPiScintilla* MainWindow::filenameToWorkspace(std::string filename)
{
    std::string s;

    for(int i = 0; i < workspace_max; i++) {
        s = "workspace_" + number_name(i);
        if(filename == s) {
            return workspaces[i];
        }
    }
    return workspaces[0];
}

void MainWindow::onExitCleanup()
{

    setupLogPathAndRedirectStdOut();
    std::cout << "[GUI] - stopping OSC server" << std::endl;
    sonicPiOSCServer->stop();
    if(protocol == TCP){
        clientSock->close();
    }
    if(serverProcess->state() == QProcess::NotRunning) {
        std::cout << "[GUI] - warning, server process is not running." << std::endl;
    } else {
        if (loaded_workspaces) {
            // this should be a synchorous call to avoid the following sleep
            saveWorkspaces();
        }
        sleep(1);
        std::cout << "[GUI] - asking server process to exit..." << std::endl;
        Message msg("/exit");
        msg.pushStr(guiID.toStdString());
        sendOSC(msg);
    }
    if(protocol == UDP){
        osc_thread.waitForFinished();
    }
    sleep(2);

    // ensure all child processes are nuked if they didn't die gracefully
    std::cout << "[GUI] - executing exit script" << std::endl;
    QProcess* exitProcess = new QProcess();
    exitProcess->start(ruby_path, QStringList(exit_script_path));
    exitProcess->waitForFinished();

    std::cout << "[GUI] - exiting. Cheerio :-)" << std::endl;
    std::cout.rdbuf(coutbuf); // reset to stdout before exiting
}

void MainWindow::heartbeatOSC() {
    // Message msg("/gui-heartbeat");
    // msg.pushStr(guiID.toStdString());
    // sendOSC(msg);
}


void MainWindow::updateDocPane(QListWidgetItem *cur) {
    QString url = cur->data(32).toString();
    docPane->setSource(QUrl(url));
}

void MainWindow::updateDocPane2(QListWidgetItem *cur, QListWidgetItem *prev) {
    (void)prev;
    updateDocPane(cur);
}

void MainWindow::addHelpPage(QListWidget *nameList,
        struct help_page *helpPages, int len) {
    int i;
    struct help_entry entry;
    entry.pageIndex = docsCentral->count()-1;

    for(i = 0; i < len; i++) {
        QListWidgetItem *item = new QListWidgetItem(helpPages[i].title);
        item->setData(32, QVariant(helpPages[i].url));
        item->setSizeHint(QSize(item->sizeHint().width(), 25));
        nameList->addItem(item);
        entry.entryIndex = nameList->count()-1;

        if (helpPages[i].keyword != NULL) {
            helpKeywords.insert(helpPages[i].keyword, entry);
            // magic numbers ahoy
            // to be revamped along with the help system
            switch (entry.pageIndex) {
                case 2:
                    autocomplete->addSymbol(SonicPiAPIs::Synth, helpPages[i].keyword);
                    break;
                case 3:
                    autocomplete->addSymbol(SonicPiAPIs::FX, helpPages[i].keyword);
                    break;
                case 5:
                    autocomplete->addKeyword(SonicPiAPIs::Func, helpPages[i].keyword);
                    break;
            }
        }
    }
}

QListWidget *MainWindow::createHelpTab(QString name) {
    QListWidget *nameList = new QListWidget;
    connect(nameList,
            SIGNAL(itemPressed(QListWidgetItem*)),
            this, SLOT(updateDocPane(QListWidgetItem*)));
    connect(nameList,
            SIGNAL(currentItemChanged(QListWidgetItem*, QListWidgetItem*)),
            this, SLOT(updateDocPane2(QListWidgetItem*, QListWidgetItem*)));

    QShortcut *up = new QShortcut(ctrlKey('p'), nameList);
    up->setContext(Qt::WidgetShortcut);
    connect(up, SIGNAL(activated()), this, SLOT(helpScrollUp()));
    QShortcut *down = new QShortcut(ctrlKey('n'), nameList);
    down->setContext(Qt::WidgetShortcut);
    connect(down, SIGNAL(activated()), this, SLOT(helpScrollDown()));

    QBoxLayout *layout = new QBoxLayout(QBoxLayout::LeftToRight);
    layout->addWidget(nameList);
    layout->setStretch(1, 1);
    QWidget *tabWidget = new QWidget;
    tabWidget->setLayout(layout);
    docsCentral->addTab(tabWidget, name);
    helpLists.append(nameList);
    return nameList;
}

void MainWindow::helpScrollUp() {
    int section = docsCentral->currentIndex();
    int entry = helpLists[section]->currentRow();

    if (entry > 0)
        entry--;
    helpLists[section]->setCurrentRow(entry);
}

void MainWindow::helpScrollDown() {
    int section = docsCentral->currentIndex();
    int entry = helpLists[section]->currentRow();

    if (entry < helpLists[section]->count()-1)
        entry++;
    helpLists[section]->setCurrentRow(entry);
}

void MainWindow::docScrollUp() {
    docPane->verticalScrollBar()->triggerAction(QAbstractSlider::SliderSingleStepSub);
}

void MainWindow::docScrollDown() {
    docPane->verticalScrollBar()->triggerAction(QAbstractSlider::SliderSingleStepAdd);
}

void MainWindow::tabNext() {
    int index = tabs->currentIndex();
    if (index == tabs->count()-1)
        index = 0;
    else
        index++;
    QMetaObject::invokeMethod(tabs, "setCurrentIndex", Q_ARG(int, index));
}

void MainWindow::tabPrev() {
    int index = tabs->currentIndex();
    if (index == 0)
        index = tabs->count() - 1;
    else
        index--;
    QMetaObject::invokeMethod(tabs, "setCurrentIndex", Q_ARG(int, index));
}

void MainWindow::setLineMarkerinCurrentWorkspace(int num) {
    if(num > 0) {
        SonicPiScintilla *ws = (SonicPiScintilla*)tabs->currentWidget();
        ws->setLineErrorMarker(num - 1);
    }
}
//TODO remove
void MainWindow::setUpdateInfoText(QString t) {
    //  update_info->setText(t);
}

void MainWindow::addUniversalCopyShortcuts(QTextEdit *te){
    new QShortcut(ctrlKey('c'), te, SLOT(copy()));
    new QShortcut(ctrlKey('a'), te, SLOT(selectAll()));

    new QShortcut(metaKey('c'), te, SLOT(copy()));
    new QShortcut(metaKey('a'), te, SLOT(selectAll()));
}

QString MainWindow::asciiArtLogo(){
    return readFile(":/images/logo.txt");
}

void MainWindow::printAsciiArtLogo(){
    QString s = asciiArtLogo();
#if QT_VERSION >= 0x050400
    qDebug().noquote() << s;
#else
    //noquote requires QT 5.4
    qDebug() << s;
#endif
}

void MainWindow::requestVersion() {
    Message msg("/version");
    msg.pushStr(guiID.toStdString());
    sendOSC(msg);
}

void MainWindow::updateVersionNumber(QString v, int v_num,QString latest_v, int latest_v_num, QDate last_checked, QString platform) {
    version = v;
    version_num = v_num;
    latest_version = latest_v;
    latest_version_num = latest_v_num;

    // update status bar
    versionLabel->setText(QString("Sonic Pi " + v + " on " + platform + " "));

    // update preferences
    QString last_update_check = tr("Last checked %1").arg(last_checked.toString());

    QString preamble = tr("Sonic Pi checks for updates\nevery two weeks.");

    QString print_version = tr("This is Sonic Pi %1");
    QString new_version = tr("Version %2 is now available!");

    if(v_num < latest_v_num) {
        QString info = QString(preamble + "\n\n" + print_version + "\n\n" + new_version).arg(version, latest_version);
        QString visit = tr("New version available!\nGet Sonic Pi %1").arg(latest_version);
        settingsWidget->updateVersionInfo( info, visit, true, false);
    }
    else {
        QString info = (preamble + "\n\n" + print_version + "\n\n" + last_update_check).arg(version);
        QString visit = tr("Visit http://sonic-pi.net to download new version");
        settingsWidget->updateVersionInfo( info, visit, false, true);
    }
}

void MainWindow::addCuePath(QString path, QString val)
{
    Q_UNUSED(val);

    if (!path.startsWith(":"))  {
        path =  "\"" + path + "\"";
    }

    if (!cuePaths.contains(path)) {
        autocomplete->addCuePath(path);
        cuePaths << path;
    }
}


void MainWindow::setupLogPathAndRedirectStdOut() {
    QDir().mkdir(sp_user_path);
    QDir().mkdir(log_path);

    if(homeDirWritable) {
        coutbuf = std::cout.rdbuf();
        stdlog.open(gui_log_path.toStdString().c_str());
        std::cout.rdbuf(stdlog.rdbuf());
    }
}

void MainWindow::toggleMidi(int silent) {
    if (piSettings->midi_enabled) {
        statusBar()->showMessage(tr("Enabling MIDI..."), 2000);
        Message msg("/midi-start");
        msg.pushStr(guiID.toStdString());
        msg.pushInt32(silent);
        sendOSC(msg);
    } else {
        settingsWidget->updateMidiInPorts(tr("No connected input devices"));
        settingsWidget->updateMidiOutPorts(tr("No connected output devices"));
        statusBar()->showMessage(tr("Disabling MIDI..."), 2000);
        Message msg("/midi-stop");
        msg.pushStr(guiID.toStdString());
        msg.pushInt32(silent);
        sendOSC(msg);
    }
}

void MainWindow::resetMidi() {
    if (piSettings->midi_enabled) {
        settingsWidget->updateMidiInPorts(tr("No connected input devices"));
        settingsWidget->updateMidiOutPorts(tr("No connected output devices"));
        statusBar()->showMessage(tr("Resetting MIDI..."), 2000);
        Message msg("/midi-reset");
        msg.pushStr(guiID.toStdString());
        sendOSC(msg);
    } else {
        statusBar()->showMessage(tr("MIDI is disabled..."), 2000);
    }
}

void MainWindow::toggleOSCServer(int silent) {
    if (piSettings->osc_server_enabled) {
        statusBar()->showMessage(tr("Opening OSC port for remote messages..."), 2000);
        int open = piSettings->osc_public ? 1 : 0;

        Message msg("/osc-port-start");
        msg.pushStr(guiID.toStdString());
        msg.pushInt32(silent);
        msg.pushInt32(open);
        sendOSC(msg);
    } else {
        statusBar()->showMessage(tr("Stopping OSC server..."), 2000);
        Message msg("/osc-port-stop");
        msg.pushStr(guiID.toStdString());
        msg.pushInt32(silent);
        sendOSC(msg);
    }
}

bool MainWindow::eventFilter(QObject *obj, QEvent *evt)
{
    if(obj==qApp && ( evt->type() == QEvent::ApplicationActivate ))
    {
        statusBar()->showMessage(tr("Welcome back. Now get your live code on..."), 2000);
        update();
    }

    // if (evt->type() == QEvent::KeyPress) {
    //     QKeyEvent *keyEvent = static_cast<QKeyEvent *>(evt);
    //     qDebug() << "Key Press: " << keyEvent->text() << " " << keyEvent->key();
    // }

    // if (evt->type() == QEvent::KeyRelease) {
    //     QKeyEvent *keyEvent = static_cast<QKeyEvent *>(evt);
    //     qDebug() << "Key Release: " << keyEvent->text();
    // }

    // if(evt->type() == QEvent::Shortcut){
    //     QShortcutEvent *sc = static_cast<QShortcutEvent *>(evt);
    //     const QKeySequence &ks = sc->key();
    //     qDebug() << "Key Shortcut: " << ks.toString();
    // }

    return QMainWindow::eventFilter(obj, evt);
}

QString MainWindow::sonicPiHomePath() {
    QString path = qgetenv("SONIC_PI_HOME").constData();
    if (path.isEmpty()) {
        return QDir::homePath();
    }
    else {
        return path;
    }
}

void MainWindow::zoomInLogs() {
    outputPane->zoomIn();
    incomingPane->zoomIn();
}

void MainWindow::zoomOutLogs() {
    outputPane->zoomOut();
    incomingPane->zoomOut();
}

void MainWindow::updateMIDIInPorts(QString port_info) {
    QString input_header = tr("Connected MIDI inputs") + ":\n\n";
    settingsWidget->updateMidiInPorts(input_header + port_info);
}

void MainWindow::updateMIDIOutPorts(QString port_info) {
    QString output_header = tr("Connected MIDI outputs") + ":\n\n";
    settingsWidget->updateMidiOutPorts(output_header + port_info);
}
