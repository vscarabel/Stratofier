/*
Stratofier Stratux AHRS Display
(c) 2018 Allen K. Lair, Sky Fun
*/

#include <QApplication>
#include <QGuiApplication>
#include <QScreen>
#include <QtDebug>
#include <QDir>
#include <QSettings>
#include <QFontDatabase>

#include "AHRSMainWin.h"
#include "Keyboard.h"
#if defined( Q_OS_ANDROID )
#include "ScreenLocker.h"
#endif


QSettings *g_pSet = nullptr;
Keyboard  *g_pKeyboard = nullptr;



int main( int argc, char *argv[] )
{
    QApplication guiApp( argc, argv );
	QStringList  qslArgs = guiApp.arguments();
    QString      qsArg;
    QString      qsToken, qsVal;
	bool         bMax = true;
    QString      qsIP;
    bool         bPortrait = true;
    AHRSMainWin *pMainWin = 0;
    QString      qsCurrWorkPath( "/home/pi/Stratofier" );  // If you put Stratofier anywhere else, specify home=<whatever> as an argument when running

#if defined( Q_OS_ANDROID )
    ScreenLocker locker;    // Keeps screen on until app exit where it's destroyed.
#endif

	foreach( qsArg, qslArgs )
    {
        QStringList qsl = qsArg.split( '=' );

        if( qsl.count() == 2 )
        {
            qsToken = qsl.first();
            qsVal = qsl.last();

            if( qsToken == "initdisp" )
                bMax = (qsVal == "max");
            else if( qsToken == "ip" )
                qsIP = qsVal;
            else if( qsToken == "orient" )
                bPortrait = (qsVal == "portrait");
            else if( qsToken == "home" )
                qsCurrWorkPath = qsVal;
        }
    }

    QFontDatabase::addApplicationFont( ":/fonts/resources/piboto_reg.ttf" );
    QFontDatabase::addApplicationFont( ":/fonts/resources/piboto_reg.ttf" );

// For Android, override any command line setting for portrait/landscape
#if defined( Q_OS_ANDROID )
    QScreen *pScreen = QGuiApplication::primaryScreen();

    QGuiApplication::setAttribute( Qt::AA_EnableHighDpiScaling );
    bPortrait = ((pScreen->orientation() == Qt::PortraitOrientation) || (pScreen->orientation() == Qt::InvertedPortraitOrientation));
// For running locally we need to set the correct working path so relative references work both locally and on the Pi
#else
    QDir::setCurrent( qsCurrWorkPath );
#endif

    QCoreApplication::setOrganizationName( "Sky Fun" );
    QCoreApplication::setOrganizationDomain( "skyfun.space" );
    QCoreApplication::setApplicationName( "Stratofier" );
    QGuiApplication::setApplicationDisplayName( "Stratofier" );

#if !defined( Q_OS_ANDROID )
    g_pSet = new QSettings( "./config.ini", QSettings::IniFormat );
#else
    g_pSet = new QSettings;
#endif

    qsIP = g_pSet->value( "StratuxIP", "192.168.10.1" ).toString();

    qInfo() << "Starting Stratofier";
    pMainWin = new AHRSMainWin( qsIP, bPortrait );
    // This is the normal mode for a dedicated Raspberry Pi touchscreen or on Android
    if( bMax )
        pMainWin->showMaximized();
    // This is only intended for running directly on the host PC without an emulator
	else
	{
        pMainWin->show();
        if( bPortrait )
            pMainWin->setGeometry( 0, 0, 640, 1100 /* 0, 0, 1024, 1638 */ );
        else
            pMainWin->setGeometry( 0, 0, 1100, 640 /* 0, 0, 1638, 1024 */ );
	}

    while( guiApp.exec() != 0 )
    {
        // For Android, override any command line setting for portrait/landscape
#if defined( Q_OS_ANDROID )
        QScreen *pScreen = QGuiApplication::primaryScreen();

        bPortrait = ((pScreen->orientation() == Qt::PortraitOrientation) || (pScreen->orientation() == Qt::InvertedPortraitOrientation));
#endif
        pMainWin = new AHRSMainWin( qsIP, bPortrait );
        pMainWin->showMaximized();
    }

    return 0;
}
