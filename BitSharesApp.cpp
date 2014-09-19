#include "BitSharesApp.hpp"

#include "html5viewer/html5viewer.h"
#include "ClientWrapper.hpp"
#include "Utilities.hpp"
#include "MainWindow.hpp"

#include <boost/thread.hpp>
#include <bts/blockchain/config.hpp>
#include <bts/wallet/url.hpp>
#include <signal.h>

#include <QSettings>
#include <QPixmap>
#include <QErrorMessage>
#include <QSplashScreen>
#include <QDir>
#include <QWebSettings>
#include <QWebPage>
#include <QWebFrame>
#include <QJsonDocument>
#include <QGraphicsWebView>
#include <QTimer>
#include <QAuthenticator>
#include <QNetworkReply>
#include <QResource>
#include <QGraphicsWidget>
#include <QMainWindow>
#include <QMenuBar>
#include <QLayout>
#include <QLocalSocket>
#include <QLocalServer>
#include <QMessageBox>
#include <QProxyStyle>
#include <QComboBox>
#include <QTemporaryFile>
#include <QTranslator>
#include <QLibraryInfo>

#include <boost/program_options.hpp>

#include <bts/client/client.hpp>
#include <bts/net/upnp.hpp>
#include <bts/blockchain/chain_database.hpp>
#include <bts/rpc/rpc_server.hpp>
#include <bts/cli/cli.hpp>
#include <bts/utilities/git_revision.hpp>
#include <fc/filesystem.hpp>
#include <fc/thread/thread.hpp>
#include <fc/log/file_appender.hpp>
#include <fc/log/logger_config.hpp>
#include <fc/io/json.hpp>
#include <fc/reflect/variant.hpp>
#include <fc/git_revision.hpp>
#include <fc/io/json.hpp>
#include <fc/log/logger_config.hpp>
#include <fc/signals.hpp>

#include <boost/iostreams/tee.hpp>
#include <boost/iostreams/stream.hpp>

#include <fstream>
#include <iostream>
#include <iomanip>

namespace
{
/// Initialized when notification about configuration loading comes
static fc::path g_P2PLogFilePath;

class TInitializationNotifier final : public ClientWrapper::INotifier
  {
  public:
    /// INotifier interface implementation:
    virtual void on_config_loaded(const bts::client::config& config) override
      {
#if defined(WIN32) && defined(USE_CRASHRPT)
      /// Reports generated by crashrpt engine should also contain newest part of p2p.log file
      for(const auto& appender : config.logging.appenders)
        {
        /// atm only p2p log is intresting and should be appended to crash report.
        if(appender.name == "p2p")
          {
          fc::file_appender::config aConfig = appender.args.as<fc::file_appender::config>();
          g_P2PLogFilePath = aConfig.filename;
          break;
          }
        }
#endif
      }
  };

/// Helper class to catch last 'n' lines of text from the stream
class TLimitedFileBuffer
  {
  public:
    typedef std::list<std::string> TStorage;
    typedef TStorage::const_iterator const_iterator;
    typedef TStorage::const_reference const_reference;
    typedef TStorage::value_type value_type;

    struct TLine
      {
      std::string text;
      operator const std::string& () const { return text; }

      friend std::istream& operator>>(std::istream& stream, TLine& l)
        {
        return std::getline(stream, l.text);
        }
      };

    explicit TLimitedFileBuffer(size_t lc) : _maxLineCount(lc) {}

    void push_back(const std::string& line)
      {
      _buffer.push_back(line);
      if(_buffer.size() > _maxLineCount)
        _buffer.pop_front();
      }

    const_iterator begin() const
      {
      return _buffer.begin();
      }

    const_iterator end() const
      {
      return _buffer.end();
      }

  private:
    size_t                 _maxLineCount;
    std::list<std::string> _buffer;
  };

} /// anonymous

// enable crashrpt win32 release only
#if defined(WIN32) && defined(USE_CRASHRPT) && defined(NDEBUG)

  #define APP_TRY /*try*/
  #define APP_CATCH /*Nothing*/

  #include "../../CrashRpt/include/CrashRpt.h"


  /* forwards SEH caught by fc's async tasks to CrashRpt */
  int unhandled_exception_filter(unsigned code, _EXCEPTION_POINTERS* info)
  { 
    return crExceptionFilter(code, info);
  }

  BOOL CALLBACK crashCallbackOld(LPVOID lpvState)
    {
    std::ifstream inputLog(g_P2PLogFilePath.generic_wstring());
    if(inputLog.good() == false)
      return TRUE;

    TLimitedFileBuffer buffer(15000);
    typedef TLimitedFileBuffer::TLine TLine;
    std::copy(std::istream_iterator<TLine>(inputLog), std::istream_iterator<TLine>(),
      std::back_inserter(buffer));

    fc::temp_file tFile;

    std::ofstream outputLog(tFile.path().generic_wstring());

    if(outputLog.good())
      std::copy(buffer.begin(), buffer.end(), std::ostream_iterator<std::string>(outputLog, "\n"));

    outputLog.close();

    std::string path = tFile.path().string();

    crAddFile2(path.c_str(), g_P2PLogFilePath.filename().string().c_str(), "P2P Log File",
      CR_AF_TAKE_ORIGINAL_FILE);

    tFile.release();

    return TRUE;
    }

  void installCrashRptHandler(const char* appName, const char* appVersion, const QFile& logFilePath)
  {
    // Define CrashRpt configuration parameters
    CR_INSTALL_INFO info = { 0 };
    info.cb = sizeof(CR_INSTALL_INFO);
    info.pszAppName = appName;
    info.pszAppVersion = appVersion;
    info.pfnCrashCallback = crashCallbackOld;
    info.pszEmailSubject = nullptr;
    info.pszEmailTo = "sales@syncad.com";
    info.pszUrl = "http://invictus.syncad.com/crash_report.html";
    info.uPriorities[CR_HTTP] = 3;  // First try send report over HTTP 
    info.uPriorities[CR_SMTP] = 2;  // Second try send report over SMTP  
    info.uPriorities[CR_SMAPI] = 1; // Third try send report over Simple MAPI    
    // Install all available exception handlers
    info.dwFlags = CR_INST_ALL_POSSIBLE_HANDLERS |
                   CR_INST_CRT_EXCEPTION_HANDLERS |
                   CR_INST_AUTO_THREAD_HANDLERS |
                   CR_INST_SEND_QUEUED_REPORTS;
    // Define the Privacy Policy URL 
    info.pszPrivacyPolicyURL = "http://invictus.syncad.com/crash_privacy.html";

    // Install crash reporting
    int nResult = crInstall(&info);
    if (nResult != 0)
    {
      // Something goes wrong. Get error message.
      char szErrorMsg[512] = { 0 };
      crGetLastErrorMsg(szErrorMsg, 512);
      elog("Cannot install CrsshRpt error handler: ${e}", ("e", szErrorMsg));
      return;
    }
    else
    {
      wlog("CrashRpt handler installed successfully");
    }

    auto logPathString = logFilePath.fileName().toStdString();

    // Add our log file to the error report
    crAddFile2(logPathString.c_str(), NULL, "Log File", CR_AF_MAKE_FILE_COPY);

    fc::variant_object version_info(bts::client::version_info());
    for (fc::variant_object::iterator version_info_iter = version_info.begin();
      version_info_iter != version_info.end(); ++version_info_iter)
    {
      std::string cr_property_name = "version_info.";
      cr_property_name += version_info_iter->key();
      crAddPropertyA(cr_property_name.c_str(), version_info_iter->value().as_string().c_str());
    }

    // We want the screenshot of the entire desktop is to be added on crash
    crAddScreenshot2(CR_AS_PROCESS_WINDOWS | CR_AS_USE_JPEG_FORMAT, 0);

    fc::set_unhandled_structured_exception_filter(&unhandled_exception_filter);
  }

  void uninstallCrashRptHandler()
  {
    crUninstall();
  }

#else // defined(WIN32) && defined(USE_CRASHRPT)

#define APP_TRY try
#define APP_CATCH \
	catch(const fc::exception& e) \
	{\
	onExceptionCaught(e);\
	}\
	catch(...)\
	{\
	onUnknownExceptionCaught();\
	}

void installCrashRptHandler(const char* appName, const char* appVersion, const QFile& logFilePath)
{
	/// Nothing to do here since no crash report support available
}

void uninstallCrashRptHandler()
{
	/// Nothing to do here since no crash report support available
}
#endif

BitSharesApp* BitSharesApp::_instance = nullptr;

#define APP_NAME "BitSharesX"

static std::string CreateBitSharesVersionNumberString()
{
  return bts::client::version_info()["client_version"].as_string();
}

QTemporaryFile gLogFile;

void ConfigureLoggingToTemporaryFile()
{
  //create log file in temporary dir that lasts after application exits
  gLogFile.setAutoRemove(false);
  gLogFile.open();
  gLogFile.close();

  //configure logger to also write to log file
  fc::file_appender::config ac;
  /** \warning Use wstring to construct log file name since %TEMP% can point to path containing
  native chars.
  */
  ac.filename = gLogFile.fileName().toStdWString();
  ac.truncate = false;
  ac.flush = true;
  fc::logger::get().add_appender(fc::shared_ptr<fc::file_appender>(new fc::file_appender(fc::variant(ac))));
}

BitSharesApp::BitSharesApp(int& argc, char** argv)
  :QApplication(argc, argv)
{
  assert(_instance == nullptr && "Only one instance allowed at time");
  _instance = this;

  QApplication::setWindowIcon(QIcon(":/images/qtapp.ico"));
}

BitSharesApp::~BitSharesApp()
{
  assert(_instance == this && "Only one instance allowed at time");
  _instance = nullptr;
}

int BitSharesApp::run(int& argc, char** argv)
{
  ConfigureLoggingToTemporaryFile();

  installCrashRptHandler(APP_NAME, CreateBitSharesVersionNumberString().c_str(), gLogFile);

  BitSharesApp app(argc, argv);
  QTranslator bitsharesTranslator;
  bitsharesTranslator.load(QLocale::system().name(), QStringLiteral(":/"));
  app.installTranslator(&bitsharesTranslator);

#ifdef __APPLE__
  QDir systemPlugins("/Library/Internet Plug-Ins");
  QDir userPlugins = QDir::home();
  userPlugins.cd("Library/Internet Plug-Ins");

  if (systemPlugins.exists("AdobeAAMDetect.plugin") || userPlugins.exists("AdobeAAMDetect.plugin")) {
    QString path = systemPlugins.exists("AdobeAAMDetect.plugin")?
                systemPlugins.absoluteFilePath("AdobeAAMDetect.plugin") : userPlugins.absoluteFilePath("AdobeAAMDetect.plugin");

    QMessageBox::warning(nullptr, tr("Adobe Application Manager Detected"),
                         tr("Warning: %1 has detected the Adobe Application Manager plug-in is installed on this "
                            "computer at %2. This plug-in crashes when loaded into %1. "
                            "Please remove this plug-in and restart %1.").arg(qApp->applicationName()).arg(path));
    return 0;
  }
#endif

  int ec = app.run();

  uninstallCrashRptHandler();

  return ec;
}

int BitSharesApp::run()
{
  setOrganizationName("DACSunlimited");
  setOrganizationDomain("dacsunlimited.com");

  setApplicationName(BTS_BLOCKCHAIN_NAME);

  //This works around Qt bug 22410, which causes a crash when repeatedly clicking a QComboBox
  class CrashWorkaroundStyle : public QProxyStyle {
  public: virtual int styleHint(StyleHint hint, const QStyleOption *option = 0,
    const QWidget *widget = 0, QStyleHintReturn *returnData = 0) const{
    if (hint == QStyle::SH_Menu_FlashTriggeredItem)
      return 0;
    return QProxyStyle::styleHint(hint, option, widget, returnData);
  }
  };
  setStyle(new CrashWorkaroundStyle);

  MainWindow mainWindow;
  bool crashedPreviously = mainWindow.detectCrash();
  installEventFilter(&mainWindow);

  //We'll go ahead and leave Win/Lin URL handling available in OSX too
  QLocalSocket* sock = new QLocalSocket();
  sock->connectToServer(BTS_BLOCKCHAIN_NAME);
  if (sock->waitForConnected(100))
  {
    if (arguments().size() > 1 && 
        arguments()[1].startsWith(QString(CUSTOM_URL_SCHEME) + ":"))
    {
      //Need to open a custom URL. Pass it to pre-existing instance.
      std::cout << "Found instance already running. Sending message and exiting." << std::endl;
      sock->write( arguments()[1].toStdString().c_str());
      sock->waitForBytesWritten();
      sock->close();
    }
    //Note that we connected, but may not have sent anything. This means that another instance is already
    //running. The fact that we connected prompted it to request focus; we will just exit now.
    delete sock;
    return 0;
  }
  else
  {
    if (arguments().size() > 1 &&
        arguments()[1].startsWith(QString(CUSTOM_URL_SCHEME) + ":"))
    {
      //No other instance running. Handle URL when we get started up.
      mainWindow.deferCustomUrl(arguments()[1]);
    }

    //Could not connect to already-running instance. Start a server so future instances connect to us
    QLocalServer* singleInstanceServer = startSingleInstanceServer(&mainWindow);
    connect(this, &QApplication::aboutToQuit, singleInstanceServer, &QLocalServer::deleteLater);
  }
  delete sock;

  auto viewer = new Html5Viewer;
  ClientWrapper* client = new ClientWrapper;
  connect(this, &QApplication::aboutToQuit, client, &ClientWrapper::close);

  if (crashedPreviously)
      client->handle_crash();

  mainWindow.setCentralWidget(viewer);
  mainWindow.setClientWrapper(client);
  mainWindow.loadWebUpdates();

  QTimer fc_tasks;
  fc_tasks.connect(&fc_tasks, &QTimer::timeout, [](){ fc::usleep(fc::microseconds(1000)); });
  fc_tasks.start(33);

  QPixmap pixmap(":/images/splash_screen.jpg");
  QSplashScreen splash(pixmap);
  splash.showMessage(QApplication::tr("Loading configuration..."),
    Qt::AlignCenter | Qt::AlignBottom, Qt::white);
  splash.show();

  prepareStartupSequence(client, viewer, &mainWindow, &splash);

  QWebSettings::globalSettings()->setAttribute(QWebSettings::PluginsEnabled, false);

  TInitializationNotifier notifier;

  APP_TRY
  {
    client->initialize(&notifier);
    return exec();
  }
  APP_CATCH
  return 0;
}

void setupMenus(ClientWrapper* client, MainWindow* mainWindow)
{
  auto accountMenu = mainWindow->accountMenu();

  accountMenu->addAction(QApplication::tr("Go to My Accounts"), mainWindow, SLOT(goToMyAccounts()), QKeySequence(QApplication::tr("Ctrl+Shift+A")));
  accountMenu->addAction(QApplication::tr("Create Account"), mainWindow, SLOT(goToCreateAccount()), QKeySequence(QApplication::tr("Ctrl+Shift+C")));
  accountMenu->addAction(QApplication::tr("Import Account"))->setEnabled(false);
  accountMenu->addAction(QApplication::tr("New Contact"), mainWindow, SLOT(goToAddContact()), QKeySequence(QApplication::tr("Ctrl+Shift+N")));
}

void BitSharesApp::prepareStartupSequence(ClientWrapper* client, Html5Viewer* viewer, MainWindow* mainWindow, QSplashScreen* splash)
{
  viewer->connect(viewer->webView(), &QGraphicsWebView::urlChanged, [viewer,client] {
    //Rebirth of the magic unicorn: When the page is reloaded, the magic unicorn dies. Make a new one.
    viewer->webView()->page()->mainFrame()->addToJavaScriptWindowObject("bitshares", client);
    viewer->webView()->page()->mainFrame()->addToJavaScriptWindowObject("magic_unicorn", new Utilities, QWebFrame::ScriptOwnership);
  });
  QObject::connect(viewer->webView()->page()->networkAccessManager(), &QNetworkAccessManager::authenticationRequired,
    [client](QNetworkReply*, QAuthenticator *auth) {
    auth->setUser(client->http_url().userName());
    auth->setPassword(client->http_url().password());
  });
  client->connect(client, &ClientWrapper::initialized, [viewer, client, mainWindow]() {
    ilog("Client initialized; loading web interface from ${url}", ("url", client->http_url().toString().toStdString()));
    client->status_update(tr("Finished connecting. Launching %1").arg(qApp->applicationName()));
    viewer->webView()->load(client->http_url());
    //Now we know the URL of the app, so we can create the items in the Accounts menu
    setupMenus(client, mainWindow);
  });
  auto loadFinishedConnection = std::make_shared<QMetaObject::Connection>();
  *loadFinishedConnection = viewer->connect(viewer->webView(), &QGraphicsWebView::loadFinished, [mainWindow, splash, viewer, loadFinishedConnection](bool ok) {
    //Workaround for wallet_get_info RPC call failure in web_wallet
    //viewer->webView()->reload();

    ilog("Webview loaded: ${status}", ("status", ok));
    viewer->disconnect(*loadFinishedConnection);
    //The web GUI takes a moment to settle; let's give it some time.
    QTimer::singleShot(100, mainWindow, SLOT(show()));
    splash->finish(mainWindow);
    mainWindow->setupTrayIcon();
    mainWindow->processDeferredUrl();
    mainWindow->checkWebUpdates(false);
  });
  client->connect(client, &ClientWrapper::error, [=](QString errorString) {
    splash->hide();
    QMessageBox::critical(nullptr, QApplication::tr("Critical Error"), errorString);
    exit(1);
  });
  client->connect(client, &ClientWrapper::status_update, [=](QString messageString) {
    splash->showMessage(messageString, Qt::AlignCenter | Qt::AlignBottom, Qt::white);
  });
}

QLocalServer* BitSharesApp::startSingleInstanceServer(MainWindow* mainWindow)
{
  QLocalServer* singleInstanceServer = new QLocalServer();
  if (!singleInstanceServer->listen(BTS_BLOCKCHAIN_NAME))
  {
    std::cerr << "Could not start new instance listener. Attempting to remove defunct listener... ";
    QLocalServer::removeServer(BTS_BLOCKCHAIN_NAME);
    if (!singleInstanceServer->listen(BTS_BLOCKCHAIN_NAME))
    {
      std::cerr << "Failed to start new instance listener: " << singleInstanceServer->errorString().toStdString() << std::endl;
      exit(1);
    }
    std::cerr << "Success.\n";
  }

  std::cout << "Listening for new instances on " << singleInstanceServer->fullServerName().toStdString() << std::endl;
  singleInstanceServer->connect(singleInstanceServer, &QLocalServer::newConnection, [singleInstanceServer, mainWindow](){
    QLocalSocket* zygote = singleInstanceServer->nextPendingConnection();
    QEventLoop waitLoop;

    zygote->connect(zygote, &QLocalSocket::readyRead, &waitLoop, &QEventLoop::quit);
    QTimer::singleShot(1000, &waitLoop, SLOT(quit()));
    waitLoop.exec();

    mainWindow->takeFocus();

    if (zygote->bytesAvailable())
    {
      QByteArray message = zygote->readLine();
      ilog("Got message from new instance: ${msg}", ("msg", message.data()));
      mainWindow->processCustomUrl(message);
    }
    zygote->close();

    delete zygote;
  });

  return singleInstanceServer;
}


bool BitSharesApp::notify(QObject* receiver, QEvent* e)
{
  APP_TRY
  {
    return QApplication::notify(receiver, e);
  }
  APP_CATCH

  return true;
}

void BitSharesApp::onExceptionCaught(const fc::exception& e)
{
  displayFailureInfo(e.to_detail_string());
}

void BitSharesApp::onUnknownExceptionCaught()
{
  std::string detail("Unknown exception caught");
  displayFailureInfo(detail);
}

void BitSharesApp::displayFailureInfo(const std::string& detail)
{
  elog("${e}", ("e", detail));
  QErrorMessage::qtHandler()->showMessage(detail.c_str());
  QApplication::quit();
}
