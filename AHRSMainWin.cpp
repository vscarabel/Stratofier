/*
Stratofier Stratux AHRS Display
(c) 2018 Allen K. Lair, Sky Fun
*/

#include <QTimer>
#include <QKeyEvent>
#include <QVBoxLayout>
#include <QPushButton>
#include <QMessageBox>
#include <QFont>
#include <QDesktopWidget>
#include <QScreen>
#include <QGuiApplication>
#include <QSettings>
#include <QPushButton>
#include <QSpacerItem>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QEventLoop>
#include <QByteArray>
#include <QDomDocument>
#include <QDomElement>
#include <QDomNode>

#include "AHRSMainWin.h"
#include "AHRSCanvas.h"
#include "MenuDialog.h"
#include "Canvas.h"
#include "Keypad.h"
#include "Builder.h"


extern QSettings *g_pSet;


// Standard fonts used throughout the app
QFont itsy(  "Droid Sans", 8,  QFont::Normal );
QFont wee(   "Droid Sans", 10, QFont::Normal );
QFont tiny(  "Droid Sans", 14, QFont::Normal );
QFont small( "Droid Sans", 16, QFont::Normal );
QFont med(   "Droid Sans", 18, QFont::Bold   );
QFont large( "Droid Sans", 24, QFont::Bold   );


Canvas::Units g_eUnitsAirspeed = Canvas::Knots;


// Setup minimal UI elements and make the connections
AHRSMainWin::AHRSMainWin( const QString &qsIP, bool bPortrait, StreamReader *pStream )
    : QMainWindow( Q_NULLPTR, Qt::Window | Qt::FramelessWindowHint ),
      m_pStratuxStream( pStream ),
      m_bStartup( true ),
      m_pMenuDialog( nullptr ),
      m_qsIP( qsIP ),
      m_bPortrait( bPortrait ),
      m_iTimerSeconds( 0 ),
      m_bTimerActive( false ),
      m_iReconnectTimer( -1 ),
      m_iTimerTimer( -1 ),
      m_bRecording( false )
{
    m_pStratuxStream->setUnits( static_cast<Canvas::Units>( g_pSet->value( "UnitsAirspeed" ).toInt() ) );

    itsy.setLetterSpacing( QFont::PercentageSpacing, 120.0 );
    wee.setLetterSpacing( QFont::PercentageSpacing, 120.0 );
    tiny.setLetterSpacing( QFont::PercentageSpacing, 120.0 );
    med.setLetterSpacing( QFont::PercentageSpacing, 120.0 );
    large.setLetterSpacing( QFont::PercentageSpacing, 120.0 );

    setupUi( this );

    m_pAHRSDisp->setPortrait( bPortrait );

    m_pSplashLabel->resize( 250 * width() / 640, 318 * height() / 800 );

    m_lastStatusUpdate = QDateTime::currentDateTime();

    connect( m_pStratuxStream, SIGNAL( newSituation( StratuxSituation ) ), m_pAHRSDisp, SLOT( situation( StratuxSituation ) ) );
    connect( m_pStratuxStream, SIGNAL( newTraffic( StratuxTraffic ) ), m_pAHRSDisp, SLOT( traffic( StratuxTraffic ) ) );
    connect( m_pStratuxStream, SIGNAL( newStatus( bool, bool, bool, bool ) ), this, SLOT( statusUpdate( bool, bool, bool, bool ) ) );
    connect( m_pStratuxStream, SIGNAL( newWTStatus( bool ) ), this, SLOT( WTUpdate( bool ) ) );

    m_pStratuxStream->connectStreams();

    QTimer::singleShot( 500, this, SLOT( init() ) );

    QScreen *pScreen = QGuiApplication::primaryScreen();
    pScreen->setOrientationUpdateMask( Qt::PortraitOrientation | Qt::InvertedPortraitOrientation | Qt::LandscapeOrientation | Qt::InvertedLandscapeOrientation );
    connect( pScreen, SIGNAL( orientationChanged( Qt::ScreenOrientation ) ), this, SLOT( orient( Qt::ScreenOrientation ) ) );

    m_iReconnectTimer = startTimer( 5000 ); // Forever timer to periodically check if we need to reconnect

    QTimer::singleShot( 5000, this, SLOT( splashOff() ) );
}


void AHRSMainWin::init()
{
    m_pAHRSDisp->orient( m_bPortrait );

    m_pStatusIndicator->setMinimumHeight( height() * (m_bPortrait ? 0.0125 : 0.025) );
}


// Delete the stream reader
AHRSMainWin::~AHRSMainWin()
{
    delete m_pStratuxStream;
    m_pStratuxStream = nullptr;

    delete m_pAHRSDisp;
    m_pAHRSDisp = nullptr;
}


void AHRSMainWin::splashOff()
{
    delete m_pSplashLabel;
    m_pSplashLabel = nullptr;
}


// Status stream is received here instead of the canvas since here is where the indicators are
void AHRSMainWin::statusUpdate( bool bStratux, bool bAHRS, bool bGPS, bool bTraffic )
{
    QString qsOn( "QLabel { border: none; background-color: LimeGreen; color: black; margin: 0px; }" );
    QString qsOff( "QLabel { border: none; background-color: LightCoral; color: black; margin: 0px; }" );

    m_pStatusIndicator->setStyleSheet( bStratux ? qsOn : qsOff );
    m_pAHRSIndicator->setStyleSheet( bAHRS ? qsOn : qsOff );
    m_pTrafficIndicator->setStyleSheet( bTraffic ? qsOn : qsOff );
    m_pGPSIndicator->setStyleSheet( bGPS ? qsOn : qsOff );

    m_lastStatusUpdate = QDateTime::currentDateTime();
}


// Valid WingThing data is being received
void AHRSMainWin::WTUpdate( bool bValid )
{
    if( bValid )
        m_pSensIndicator->setStyleSheet( "QLabel { border: none; background-color: LimeGreen; color: black; margin: 0px; }" );
    else
        m_pSensIndicator->setStyleSheet( "QLabel { border: none; background-color: LightCoral; color: black; margin: 0px; }" );
}


// Display the menu dialog and handle specific returns
void AHRSMainWin::menu()
{
    if( m_pMenuDialog == nullptr )
    {
        CanvasConstants c = m_pAHRSDisp->canvas()->constants();

        m_pMenuDialog = new MenuDialog( this, m_bPortrait, m_bRecording );

        // Scale the menu dialog according to screen resolution
        m_pMenuDialog->setMinimumWidth( static_cast<int>( c.dW4 ) );
        m_pMenuDialog->setGeometry( c.dW - c.dW2, 0.0, static_cast<int>( c.dW2 ), static_cast<int>( c.dH ) );
        m_pAHRSDisp->dark( true );
        m_pMenuDialog->show();
        connect( m_pMenuDialog, SIGNAL( resetLevel() ), this, SLOT( resetLevel() ) );
        connect( m_pMenuDialog, SIGNAL( resetGMeter() ), this, SLOT( resetGMeter() ) );
        connect( m_pMenuDialog, SIGNAL( upgradeStratofier() ), this, SLOT( upgradeStratofier() ) );
        connect( m_pMenuDialog, SIGNAL( shutdownStratux() ), this, SLOT( shutdownStratux() ) );
        connect( m_pMenuDialog, SIGNAL( shutdownStratofier() ), this, SLOT( shutdownStratofier() ) );
        connect( m_pMenuDialog, SIGNAL( timer() ), this, SLOT( changeTimer() ) );
        connect( m_pMenuDialog, SIGNAL( fuelTanks( FuelTanks ) ), this, SLOT( fuelTanks( FuelTanks ) ) );
        connect( m_pMenuDialog, SIGNAL( stopFuelFlow() ), this, SLOT( stopFuelFlow() ) );
        connect( m_pMenuDialog, SIGNAL( unitsAirspeed() ), this, SLOT( unitsAirspeed() ) );
        connect( m_pMenuDialog, SIGNAL( setSwitchableTanks( bool ) ), this, SLOT( setSwitchableTanks( bool ) ) );
        connect( m_pMenuDialog, SIGNAL( settingsClosed() ), this, SLOT( settingsClosed() ) );
        connect( m_pMenuDialog, SIGNAL( magDev( int ) ), this, SLOT( magDev( int ) ) );
        connect( m_pMenuDialog, SIGNAL( recordFlight( bool ) ), this, SLOT( recordFlight( bool ) ) );
    }
    else
    {
        delete m_pMenuDialog;
        m_pMenuDialog = nullptr;
    }
}


void AHRSMainWin::resetLevel()
{
    emptyHttpPost( "cageAHRS" );
    m_pStratuxStream->snapshotOrientation();
}


void AHRSMainWin::resetGMeter()
{
    emptyHttpPost( "resetGMeter" );
}


void AHRSMainWin::emptyHttpPost( const QString &qsToken )
{
    QNetworkAccessManager manager;

    manager.post( QNetworkRequest( QUrl( QString( "http://%1/%2" ).arg( m_qsIP ).arg( qsToken ) ) ), QByteArray() );

    QNetworkReply        *pResp = manager.get( QNetworkRequest( QUrl( QString( "http://%1/cageAHRS" ).arg( m_qsIP ) ) ) );
    QEventLoop            eventloop;

    connect( pResp, SIGNAL( finished() ), &eventloop, SLOT( quit() ) );
    eventloop.exec();

    QString throwaway = pResp->readAll();

    delete m_pMenuDialog;
    m_pMenuDialog = nullptr;
}


void AHRSMainWin::upgradeStratofier()
{
    delete m_pMenuDialog;
    m_pMenuDialog = nullptr;
    if( QMessageBox::question( this, "UPGRADE", "Upgrading Stratofier requires an active internet connection.\n\n"
                                                "Select 'Yes' to download and install the latest Stratofier version.",
                               QMessageBox::Yes, QMessageBox::No ) == QMessageBox::Yes )
    {
        if( system( "/home/pi/Stratofier/upgrade.sh > /dev/null 2>&1 &" ) != 0 )
            qDebug() << "Error executing upgrade script.";
        QApplication::processEvents();
        qApp->closeAllWindows();
    }
}


void AHRSMainWin::shutdownStratofier()
{
    qApp->exit( 0 );
}


void AHRSMainWin::shutdownStratux()
{
    qApp->exit( 0 );
    if( system( "sudo shutdown -h now" ) != 0 )
        qDebug() << "Error shutting down.";
}


// Use the escape key to close the app since there is no title bar
void AHRSMainWin::keyReleaseEvent( QKeyEvent *pEvent )
{
    if( pEvent->key() == Qt::Key_Escape )
        qApp->closeAllWindows();
    pEvent->accept();
}


void AHRSMainWin::timerEvent( QTimerEvent *pEvent )
{
    if( pEvent == Q_NULLPTR )
        return;

    // If we haven't gotten a status update for over ten seconds, force a reconnect
    if( pEvent->timerId() == m_iReconnectTimer )
    {
        if( m_lastStatusUpdate.secsTo( QDateTime::currentDateTime() ) > 10 )
        {
            m_pStratuxStream->disconnectStreams();
            QTimer::singleShot( 1000, m_pStratuxStream, SLOT( connectStreams() ) );
        }
    }
    else if( pEvent->timerId() == m_iTimerTimer )
    {
        QDateTime now = QDateTime::currentDateTime();
        int       iTimer = m_iTimerSeconds - static_cast<int>( m_timerStart.secsTo( now ) );
        int       iMinutes = iTimer / 60;
        int       iSeconds = iTimer - (iMinutes * 60);

        m_pAHRSDisp->timerReminder( iMinutes, iSeconds );
    }
}


void AHRSMainWin::changeTimer()
{
    Keypad keypad( this, "TIMER", true );

    m_pAHRSDisp->canvas()->setKeypadGeometry( &keypad );

    delete m_pMenuDialog;
    m_pMenuDialog = nullptr;

    if( keypad.exec() == QDialog::Accepted )
    {
        QString qsTime = keypad.textValue();
    
        m_timerStart = QDateTime::currentDateTime();
        m_iTimerSeconds = (qsTime.left( 2 ).toInt() * 60) + qsTime.right( 2 ).toInt();
        m_iTimerTimer = startTimer( 500 );
        m_bTimerActive = true;
    }
    else
        stopTimer();
}


void AHRSMainWin::restartTimer()
{
    m_timerStart = QDateTime::currentDateTime();
    m_bTimerActive = true;
}


void AHRSMainWin::stopTimer()
{
    if( m_iTimerTimer == -1 )
        killTimer( m_iTimerTimer );
    m_pAHRSDisp->timerReminder( -1, -1 );
    m_iTimerTimer = -1;
    m_bTimerActive = false;
}


void AHRSMainWin::orient( Qt::ScreenOrientation o )
{
    m_pAHRSDisp->orient( (o == Qt::PortraitOrientation) || (o == Qt::InvertedPortraitOrientation) );
}


void AHRSMainWin::fuelTanks( FuelTanks tanks )
{
    if( !tanks.bDualTanks )
    {
        tanks.dRightCapacity = 0.0;
        tanks.dRightRemaining = 0.0;
    }
    m_pAHRSDisp->setFuelTanks( tanks );
    m_pAHRSDisp->m_bFuelFlowStarted = true;
    QTimer::singleShot( 10, this, SLOT( fuelTanks2() ) );
}


void AHRSMainWin::fuelTanks2()
{
    delete m_pMenuDialog;
    m_pMenuDialog = nullptr;
}


void AHRSMainWin::stopFuelFlow()
{
    m_pAHRSDisp->m_bFuelFlowStarted = false;
    QTimer::singleShot( 10, this, SLOT( fuelTanks2() ) );
}


void AHRSMainWin::unitsAirspeed()
{
    MenuDialog *pDlg = static_cast<MenuDialog *>( sender() );
    int         i = static_cast<int>( g_eUnitsAirspeed );

    i++;
    if( i > static_cast<int>( Canvas::KPH ) )
        i = static_cast<int>( Canvas::MPH );
    g_eUnitsAirspeed = static_cast<Canvas::Units>( i );
    if( pDlg != nullptr )
    {
        if( g_eUnitsAirspeed == Canvas::MPH )
            pDlg->m_pUnitsAirspeedButton->setText( "MPH" );
        else if( g_eUnitsAirspeed == Canvas::Knots )
            pDlg->m_pUnitsAirspeedButton->setText( "KNOTS" );
        else
            pDlg->m_pUnitsAirspeedButton->setText( "KPH" );
    }
    g_pSet->setValue( "UnitsAirspeed", static_cast<int>( g_eUnitsAirspeed ) );
    g_pSet->sync();
    m_pStratuxStream->setUnits( g_eUnitsAirspeed );
    m_pAHRSDisp->update();
}


void AHRSMainWin::setSwitchableTanks( bool bSwitchable )
{
    m_pAHRSDisp->setSwitchableTanks( bSwitchable );
}


void AHRSMainWin::magDev( int iMagDev )
{
    m_pAHRSDisp->setMagDev( iMagDev );
}


void AHRSMainWin::recordFlight( bool bRec )
{

    if( bRec )
        m_Track.clear();
    else
    {
        QString qsInternal;
        Airport ap = TrafficMath::getCurrentAirport();

        Builder::getStorage( &qsInternal );
        qsInternal.append( QString( "/data/space.skyfun.stratofier/Stratofier_%1_%2.kml" ).arg( ap.qsID ).arg( QDateTime::currentDateTime().toString( Qt::ISODate ).remove( ':' ).remove( '-' ) ) );

        TrackPoint   tp;
        QDomDocument kml;
        QDomElement  xRoot, xDoc, xPlacemark, xStyle, xP, xL;
        QDomNode     xDocNode, xPlacemarkNode, xStyleNode;
        QDomText     nameText;
        QFile        track( qsInternal );
        QString      qsTrack;

        kml.setContent( QString( "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                                 "<kml xmlns=\"http://www.opengis.net/kml/2.2\" xmlns:gx=\"http://www.google.com/kml/ext/2.2\" xmlns:kml=\"http://www.opengis.net/kml/2.2\" xmlns:atom=\"http://www.w3.org/2005/Atom\">\n"
                                 "<Document>\n"
                                 "<Style id=\"stratofier_track\"><LineStyle><color>ff0055ff</color><width>3</width></LineStyle></Style>\n"
                                 "<StyleMap id=\"m_stratofier_track\"><Pair><key>normal</key><styleUrl>stratofier_track</styleUrl></Pair></StyleMap>\n"
                                 "</Document>\n"
                                 "</kml>\n" ).toLatin1() );

        if( track.open( QIODevice::WriteOnly ) )
        {
            xRoot = kml.documentElement();  // <klm>
            xDocNode = xRoot.firstChild();  // <Document>
            xDoc = xDocNode.toElement();
            xPlacemark = kml.createElement( "name" );
            nameText = kml.createTextNode( QString( "Stratofier_%1_%2.kml" ).arg( ap.qsID ).arg( QDateTime::currentDateTime().toString( Qt::ISODate ).remove( ':' ).remove( '-' ) ) );
            xPlacemark.appendChild( nameText );
            xStyleNode = xDoc.firstChild();
            xDoc.insertBefore( xPlacemarkNode, xStyleNode );

            xPlacemark = kml.createElement( "Placemark" );
            xDoc.appendChild( xPlacemark );

            xP = kml.createElement( "name" );
            nameText = kml.createTextNode( "Track" );
            xP.appendChild( nameText );
            xPlacemark.appendChild( xP );

            xP = kml.createElement( "description" );
            nameText = kml.createTextNode( "Recorded by Stratofier" );
            xP.appendChild( nameText );
            xPlacemark.appendChild( xP );

            xP = kml.createElement( "styleUrl" );
            nameText = kml.createTextNode( "#m_stratofier_track" );
            xP.appendChild( nameText );
            xPlacemark.appendChild( xP );

            xL = kml.createElement( "LineString" );
            xPlacemark.appendChild( xL );

            xP = kml.createElement( "extrude" );
            nameText = kml.createTextNode( "1" );
            xP.appendChild( nameText );
            xL.appendChild( xP );

            xP = kml.createElement( "tesselate" );
            nameText = kml.createTextNode( "1" );
            xP.appendChild( nameText );
            xL.appendChild( xP );

            xP = kml.createElement( "altitudeMode" );
            nameText = kml.createTextNode( "absolute" );
            xP.appendChild( nameText );
            xL.appendChild( xP );

            foreach( tp, m_Track )
                qsTrack.append( QString( "%1,%2,%3\n" ).arg( tp.dLat ).arg( tp.dLong ).arg( tp.dAlt ) );

            xP = kml.createElement( "coordinates" );
            nameText = kml.createTextNode( qsTrack );
            xP.appendChild( nameText );
            xL.appendChild( xP );

            track.write( kml.toString( 4 ).toLatin1() );
            track.close();
        }
    }

    m_bRecording = bRec;
}


void AHRSMainWin::settingsClosed()
{
    QTimer::singleShot( 100, this, SLOT( menu() ) );
}

