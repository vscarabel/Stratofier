/*
Stratofier Stratux AHRS Display
(c) 2018 Allen K. Lair, Sky Fun
*/

#include <QPainter>
#include <QtDebug>
#include <QMouseEvent>
#include <QTimer>
#include <QFont>
#include <QLinearGradient>
#include <QLineF>
#include <QSettings>
#include <QtConcurrent>
#include <QTransform>
#include <QVariant>
#include <QScreen>
#include <QGesture>
#include <QGestureEvent>
#include <QSwipeGesture>
#include <QPinchGesture>
#include <QBitmap>

#include <math.h>

#include "AHRSCanvas.h"
#include "BugSelector.h"
#include "Keypad.h"
#include "AHRSMainWin.h"
#include "StreamReader.h"
#include "Builder.h"
#include "StratofierDefs.h"
#include "TimerDialog.h"
#include "AirportDialog.h"
#include "DetailsDialog.h"
#include "Overlays.h"


extern QFont itsy;
extern QFont wee;
extern QFont tiny;
extern QFont small;
extern QFont med;
extern QFont large;

extern bool g_bDayMode;

extern QList<Airport>  g_airportCache;
extern QList<Airspace> g_airspaceCache;

extern QSettings *g_pSet;

StratuxSituation          g_situation;
QMap<int, StratuxTraffic> g_trafficMap;

extern Canvas::Units g_eUnitsAirspeed;

bool g_bNoAirportsUpdate = false;

QString g_qsStratofierVersion( "1.8.2.0" );

QFuture<void> g_apt;

/*
IMPORTANT NOTE:

Wherever the dH constant is used for scaling, even for width or x offset, was deliberate, since
that constant is always the same regardless of whether we're in landscape or portrait mode.  The
dWa constant is used differently which is why the more convenient dH is used instead.
*/


AHRSCanvas::AHRSCanvas( QWidget *parent )
    : QWidget( parent ),
      m_bFuelFlowStarted( false ),
      m_pCanvas( Q_NULLPTR ),
      m_bInitialized( false ),
      m_iHeadBugAngle( -1 ),
      m_iWindBugAngle( -1 ),
      m_iWindBugSpeed( 0 ),
      m_iAltBug( -1 ),
      m_bUpdated( false ),
      m_bShowGPSDetails( false ),
      m_bPortrait( true ),
      m_bLongPress( false ),
      m_longPressStart( QDateTime::currentDateTime() ),
      m_bShowCrosswind( false ),
      m_iTimerMin( -1 ),
      m_iTimerSec( -1 ),
      m_iMagDev( 0 ),
      m_bDisplayTanksSwitchNotice( false ),
      m_SwipeStart( 0, 0 ),
      m_iSwiping( 0 ),
      m_tanks( { 0.0, 0.0, 0.0, 0.0, 9.0, 10.0, 8.0, 5.0, 30, true, true, QDateTime::currentDateTime() } ),
      m_dBaroPress( 29.92 )
{
    m_directAP.qsID = "NULL";
    m_directAP.qsName = "NULL";
    m_fromAP.qsID = "NULL";
    m_fromAP.qsName = "NULL";
    m_toAP.qsID = "NULL";
    m_toAP.qsName = "NULL";

    // Initialize AHRS settings
    // No need to init the traffic because it starts out as an empty QMap.
    StreamReader::initSituation( g_situation );

    // Preload the fancier icons that are impractical to paint programmatically
    m_planeIcon.load( ":/graphics/resources/Plane.png" );
    m_headIcon.load( ":/icons/resources/HeadingIcon.png" );
    m_windIcon.load( ":/icons/resources/WindIcon.png" );
    m_DirectTo.load( ":/graphics/resources/DirectTo.png" );
    m_FromTo.load( ":/graphics/resources/FromTo.png" );
    m_AltBug.load( ":/icons/resources/AltBug.png" );
    m_directIcon.load( ":/icons/resources/DirectIcon.png" );

    loadSettings();

    // Quick and dirty way to ensure we're shown full screen before any calculations happen
    QTimer::singleShot( 2000, this, SLOT( init() ) );
}


// Delete everything that needs deleting
AHRSCanvas::~AHRSCanvas()
{
    if( g_pSet != Q_NULLPTR )
    {
        g_pSet->sync();
        delete g_pSet;
        g_pSet = Q_NULLPTR;
    }
    if( m_pCanvas != Q_NULLPTR )
    {
        delete m_pCanvas;
        m_pCanvas = Q_NULLPTR;
    }
}


// Create the canvas utility instance, create the various pixmaps that are built up for fast painting
// and start the update timer.
void AHRSCanvas::init()
{
    if( m_pCanvas != Q_NULLPTR )
        delete m_pCanvas;

    m_pCanvas = new Canvas( width(), height(), m_bPortrait );

    CanvasConstants c = m_pCanvas->constants();
    int             iBugSize = static_cast<int>( c.dWa * (m_bPortrait ? 0.1333 : 0.08) );

    m_headIcon = m_headIcon.scaled( iBugSize, iBugSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation );
    m_windIcon = m_windIcon.scaled( iBugSize, iBugSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation );

    if( m_bPortrait )
        m_VertSpeedTape.load( ":/graphics/resources/vspeedP.png" );
    else
        m_VertSpeedTape.load( ":/graphics/resources/vspeedL.png" );

    m_HeadIndicator.load( ":/graphics/resources/headBG.png" );
    m_HeadIndicatorOverlay.load( ":/graphics/resources/head.png" );
    m_RollIndicator.load( ":/graphics/resources/roll.png" );
    m_AltTape.load( ":/graphics/resources/alttape.png" );
    m_Lfuel.load( ":/graphics/resources/fuel.png" );
    m_SpeedTape.load( ":/graphics/resources/speedtape.png" );
    m_AltTape = m_AltTape.scaledToWidth( c.dW10, Qt::SmoothTransformation );
    m_SpeedTape = m_SpeedTape.scaledToWidth( c.dW10 - c.dW40, Qt::SmoothTransformation );
    QTransform flipper;
    flipper.scale( -1, 1 );
    m_Rfuel = m_Lfuel.transformed( flipper );

    m_iDispTimer = startTimer( 5000 );     // Update the in-memory airspace objects every 15 seconds

    QtConcurrent::run( TrafficMath::cacheAirports );
    QtConcurrent::run( TrafficMath::cacheAirspaces );

    m_bInitialized = true;
}


void AHRSCanvas::loadSettings()
{
    m_dZoomNM = g_pSet->value( "ZoomNM", 10.0 ).toDouble();
    m_settings.bShowAllTraffic = g_pSet->value( "ShowAllTraffic", true ).toBool();
    m_settings.eShowAirports = static_cast<Canvas::ShowAirports>( g_pSet->value( "ShowAirports", 2 ).toInt() );
    m_settings.bShowRunways = g_pSet->value( "ShowRunways", true ).toBool();
    m_settings.bShowAirspaces = g_pSet->value( "ShowAirspaces", true ).toBool();
    m_settings.bShowAltitudes = g_pSet->value( "ShowAltitudes", true ).toBool();
    m_iMagDev = g_pSet->value( "MagDev", 0 ).toInt();
    g_eUnitsAirspeed = m_settings.eUnits = static_cast<Canvas::Units>( g_pSet->value( "UnitsAirspeed", true ).toInt() );

    g_pSet->beginGroup( "FuelTanks" );
    m_tanks.dLeftCapacity = g_pSet->value( "LeftCapacity", 24.0 ).toDouble();
    m_tanks.dRightCapacity = g_pSet->value( "RightCapacity", 24.0 ).toDouble();
    m_tanks.dLeftRemaining = g_pSet->value( "LeftRemaining", 24.0 ).toDouble();
    m_tanks.dRightRemaining = g_pSet->value( "RightRemaining", 24.0 ).toDouble();
    m_tanks.dFuelRateCruise = g_pSet->value( "CruiseRate", 8.3 ).toDouble();
    m_tanks.dFuelRateClimb = g_pSet->value( "ClimbRate", 9.0 ).toDouble();
    m_tanks.dFuelRateDescent = g_pSet->value( "DescentRate", 7.0 ).toDouble();
    m_tanks.dFuelRateTaxi = g_pSet->value( "TaxiRate", 4.0 ).toDouble();
    m_tanks.iSwitchIntervalMins = g_pSet->value( "SwitchInterval", 15 ).toInt();
    g_pSet->endGroup();
}


// Just a utility timer that periodically updates the display when it's not being driven by the streams
// coming from the Stratux.
void AHRSCanvas::timerEvent( QTimerEvent *pEvent )
{
    if( pEvent == 0 )
        return;

    QDateTime qdtNow = QDateTime::currentDateTime();

    m_settings.eShowAirports = static_cast<Canvas::ShowAirports>( g_pSet->value( "ShowAirports", 2 ).toInt() );
    update();

    cullTrafficMap();

    // If we have a valid GPS position, run the list of airports within range by threading it so it doesn't interfere with the display update
    if( (g_situation.dGPSlat != 0.0) && (g_situation.dGPSlong != 0.0) )
    {
        // This threaded function will crash the app if we're accessing the airports cache in the middle of an update
        if( !g_bNoAirportsUpdate )
            g_apt = QtConcurrent::run( TrafficMath::updateNearbyAirports, &m_airports, &m_directAP, &m_fromAP, &m_toAP, m_dZoomNM );
        QtConcurrent::run( TrafficMath::updateNearbyAirspaces, &m_airspaces, m_dZoomNM );
    }

    if( m_bFuelFlowStarted )
    {
        double dInterval = 0.00416666666667;   // 15 seconds / 3600 seconds (1 hour); 15 seconds is the interval of the one and only timer

        // If the aircraft is moving slowly and there is virtually no vertical movement then we are taxiing
        if( (g_situation.dGPSGroundSpeed > 5.0) && (g_situation.dGPSGroundSpeed < 20.0) && (fabs( g_situation.dBaroVertSpeed ) < 5.0) )
        {
            if( m_tanks.bOnLeftTank || (!m_tanks.bDualTanks) )
                m_tanks.dLeftRemaining -= (m_tanks.dFuelRateTaxi * dInterval);
            else
                m_tanks.dRightRemaining -= (m_tanks.dFuelRateTaxi * dInterval);
        }
        // If we're moving faster than 35 knots and we're at least anemically climbing, then use the climb rate
        else if( (g_situation.dGPSGroundSpeed > 35.0) && (g_situation.dBaroVertSpeed > 50.0) )
        {
            if( m_tanks.bOnLeftTank || (!m_tanks.bDualTanks) )
                m_tanks.dLeftRemaining -= (m_tanks.dFuelRateClimb * dInterval);
            else
                m_tanks.dRightRemaining -= (m_tanks.dFuelRateClimb * dInterval);
        }
        // If we're at least at an anemic airspeed and reasonbly acceptable altitude control, then use the cruise rate
        else if( (g_situation.dGPSGroundSpeed > 70.0) && (g_situation.dBaroVertSpeed < 100.0) )
        {
            if( m_tanks.bOnLeftTank || (!m_tanks.bDualTanks) )
                m_tanks.dLeftRemaining -= (m_tanks.dFuelRateCruise * dInterval);
            else
                m_tanks.dRightRemaining -= (m_tanks.dFuelRateCruise * dInterval);
        }
        // If we're in at least a slow descent, use the descent rate
        else if( (g_situation.dGPSGroundSpeed > 70.0) && (g_situation.dBaroVertSpeed < -250.0) )
        {
            if( m_tanks.bOnLeftTank || (!m_tanks.bDualTanks) )
                m_tanks.dLeftRemaining -= (m_tanks.dFuelRateDescent * dInterval);
            else
                m_tanks.dRightRemaining -= (m_tanks.dFuelRateDescent * dInterval);
        }

        if( (m_tanks.lastSwitch.secsTo( qdtNow ) > (m_tanks.iSwitchIntervalMins * 60)) && m_tanks.bDualTanks )
            m_bDisplayTanksSwitchNotice = true;
    }

    m_bUpdated = false;
}


// Where all the magic happens
void AHRSCanvas::paintEvent( QPaintEvent *pEvent )
{
    if( (!m_bInitialized) || (pEvent == 0) )
        return;

    if( m_bPortrait )
        paintPortrait();
    else
        paintLandscape();
}


// Draw the traffic onto the heading indicator and the tail numbers on the side
void AHRSCanvas::updateTraffic( QPainter *pAhrs, CanvasConstants *c )
{
    QList<StratuxTraffic> trafficList = g_trafficMap.values();
    StratuxTraffic        traffic;
    QPen                  planePen( Qt::black, 15, Qt::SolidLine, Qt::RoundCap, Qt::BevelJoin );
    double				  dPxPerNM = static_cast<double>( c->dW - 30.0 ) / (m_dZoomNM * 2.0);	// Pixels per nautical mile; the outer limit of the heading indicator is calibrated to the zoom level in NM
    QLineF				  track, ball, info;
	double                dAlt;
	QString               qsSign;
    QFontMetrics          smallMetrics( small );
    int                   iBallPenWidth = static_cast<int>( c->dH * (m_bPortrait ? 0.01875 : 0.03125) );
    int                   iCourseLinePenWidth = static_cast<int>( c->dH * (m_bPortrait ? 0.00625 : 0.010417) );
    int                   iCourseLineLength = static_cast<int>( c->dH * (m_bPortrait ? 0.0375 : 0.0625) );
    QPainterPath          maskPath;
    QColor                closenessColor( Qt::green );
    double                deltaY;

    maskPath.addEllipse( 10.0 + (m_bPortrait ? 0 : c->dW),
                         c->dH - c->dW,
                         c->dW - c->dW20, c->dW - c->dW20 );
    pAhrs->setClipPath( maskPath );

    // Draw a large dot for each aircraft; the outer edge of the heading indicator is calibrated to be 20 NM out from your position
	foreach( traffic, trafficList )
    {
        // If bearing and distance were able to be calculated then show relative position
        if( traffic.bHasADSB && (traffic.qsTail != m_settings.qsOwnshipID) )
        {
			double dTrafficDist = traffic.dDist * dPxPerNM;
            double dAltDist = traffic.dAlt - g_situation.dBaroPressAlt;
            double dAltDistAbs = fabs( dAltDist );

            if( m_settings.bShowAllTraffic || (dAltDistAbs < 5000) )
            {
                closenessColor = Qt::green;

                if( traffic.bOnGround )
                    closenessColor = Qt::cyan;
                else if( (dAltDistAbs <= 2000) && (dAltDistAbs > 1000) )
                    closenessColor = Qt::yellow;
                else if( (dAltDistAbs <= 1000) && (dAltDistAbs > 500) )
                    closenessColor = QColor( 0xFF, 0xA5, 0x00 );
                else if( dAltDistAbs <= 500 )
                    closenessColor = Qt::red;

                ball.setP1( QPointF( (m_bPortrait ? 0 : c->dW) + c->dW2, c->dH - c->dW2 - 30.0 ) );
                ball.setP2( QPointF( (m_bPortrait ? 0 : c->dW) + c->dW2, c->dH - c->dW2 - 30.0 - dTrafficDist ) );

			    // Traffic angle in reference to you (which clock position they're at)
                if( g_situation.bHaveWTData )
                    ball.setAngle( g_situation.dAHRSMagHeading - traffic.dBearing + 90.0 );
                else
                    ball.setAngle( g_situation.dAHRSGyroHeading - traffic.dBearing + 90.0 );
                // Qt Y coords are backward
                deltaY = ball.p2().y() - (c->dH - c->dW2 - 30.0);
                ball.setP2( QPointF( ball.p2().x(), c->dH - c->dW2 - 30.0 + deltaY ) );

			    // Draw the black part of the track line
                planePen.setWidth( static_cast<int>( c->dH * (m_bPortrait ? 0.00625 : 0.010417) ) );
                planePen.setColor( Qt::black );
			    track.setP1( ball.p2() );
                track.setP2( QPointF( ball.p2().x(), ball.p2().y() + iCourseLineLength ) );
                track.setAngle( g_situation.dAHRSGyroHeading - traffic.dTrack  + 90.0 );
                pAhrs->setPen( planePen );
			    pAhrs->drawLine( track );

                // Draw the dot
                planePen.setWidth( iBallPenWidth );
                planePen.setColor( Qt::black );
                pAhrs->setPen( planePen );
                pAhrs->drawPoint( ball.p2() );
                planePen.setColor( closenessColor );
                pAhrs->setPen( planePen );
                pAhrs->drawPoint( ball.p2().x() - 2, ball.p2().y() - 2 );

                // Draw the green part of the track line
                planePen.setWidth( iCourseLinePenWidth );
                planePen.setColor( closenessColor );
                track.setP1( QPointF( ball.p2().x() - 2, ball.p2().y() - 2 ) );
                track.setP2( QPointF( ball.p2().x() - 2, ball.p2().y() + iCourseLineLength - 2 ) );
                track.setAngle( g_situation.dAHRSGyroHeading - traffic.dTrack + 90.0 );
                pAhrs->setPen( planePen );
                pAhrs->drawLine( track );

			    // Draw the ID, numerical track heading and altitude delta
			    dAlt = (traffic.dAlt - g_situation.dBaroPressAlt) / 100.0;
			    if( dAlt > 0 )
				    qsSign = "+";
			    else if( dAlt < 0 )
				    qsSign = "-";
			    pAhrs->setPen( Qt::black );
                pAhrs->setFont( wee );
                info.setP1( ball.p2() );
                info.setP2( QPointF( ball.p2().x() + c->dW40, ball.p2().y() ) );
                info.setAngle( 30.0 );
                pAhrs->drawText( info.p2(), traffic.qsTail.isEmpty() ? "UNKWN" : traffic.qsTail );
                info.setAngle( -50.0 );
                pAhrs->drawText( info.p2(), QString( "%1%2" ).arg( qsSign ).arg( static_cast<int>( fabs( dAlt ) ) ) );
                pAhrs->setPen( closenessColor );
                info.translate( 2.0, 2.0 );
                info.setAngle( 30.0 );
                pAhrs->drawText( info.p2(), traffic.qsTail.isEmpty() ? "UNKWN" : traffic.qsTail );
                info.setAngle( -50.0 );
                pAhrs->drawText( info.p2(), QString( "%1%2" ).arg( qsSign ).arg( static_cast<int>( fabs( dAlt ) ) ) );
            }
		}
    }

    pAhrs->setClipping( false );

    // TODO: Move these out to their own painters

    QString qsZoom = QString( "%1nm" ).arg( static_cast<int>( m_dZoomNM ) );
    QString qsMagDev = QString( "%1%2" ).arg( m_iMagDev ).arg( QChar( 0xB0 ) );

    if( m_iMagDev < 0 )
        qsMagDev.prepend( "   " );  // There will already be a negative symbol
    else if( m_iMagDev > 0 )
        qsMagDev.prepend( "   +" );
    else
        qsMagDev.prepend( "   " );

    // Draw the zoom level
    pAhrs->setFont( tiny );
    pAhrs->setPen( Qt::black );
    QLineF zoomLine = (m_bPortrait ? QLineF( c->dW2, c->dH - c->dW2 - 20.0, c->dW, c->dH - c->dW2 - 20.0 )
                                   : QLineF( c->dW + c->dW2, c->dH - c->dW2 - 10.0, c->dWa, c->dH - c->dW2 - 10.0 ));
    zoomLine.setAngle( -40.0 );
    pAhrs->drawText( zoomLine.p2().x() + 2.0, zoomLine.p2().y() + 2.0, qsZoom );
    pAhrs->setPen( QColor( 80, 255, 80 ) );
    pAhrs->drawText( zoomLine.p2(), qsZoom );

    // Draw the magnetic deviation
    zoomLine.setAngle( -50.0 );
    pAhrs->setPen( Qt::black );
    pAhrs->drawText( zoomLine.p2().x() + 2.0, zoomLine.p2().y() + 2.0, qsMagDev );
    pAhrs->setPen( Qt::yellow );
    pAhrs->drawText( zoomLine.p2(), qsMagDev );
}


// Situation (mostly AHRS data) update
void AHRSCanvas::situation( StratuxSituation s )
{
    g_situation = s;
    g_situation.dAHRSGyroHeading += static_cast<double>( m_iMagDev );
    g_situation.dAHRSMagHeading += static_cast<double>( m_iMagDev );
    if( g_situation.dAHRSGyroHeading < 0.0 )
        g_situation.dAHRSGyroHeading += 360.0;
    else if( g_situation.dAHRSGyroHeading > 360.0 )
        g_situation.dAHRSGyroHeading -= 360.0;
    if( g_situation.dAHRSMagHeading < 0.0 )
        g_situation.dAHRSMagHeading += 360.0;
    else if( g_situation.dAHRSMagHeading > 360.0 )
        g_situation.dAHRSMagHeading -= 360.0;
    m_bUpdated = true;
    update();
}


// Traffic update
void AHRSCanvas::traffic( int iICAO, StratuxTraffic t )
{
    g_trafficMap.insert( iICAO, t );
    m_bUpdated = true;
    update();
}


void AHRSCanvas::cullTrafficMap()
{
    if( g_trafficMap.count() == 0 )
        return;

    g_trafficMap.clear();
/*
    QMapIterator<int, StratuxTraffic> it( g_trafficMap );
    bool                              bTrafficRemoved = true;
    QDateTime                         now = QDateTime::currentDateTime();

    // Each time this is updated, remove an old entry
    while( bTrafficRemoved )
    {
        bTrafficRemoved = false;
        while( it.hasNext() )
        {
            it.next();
            // Anything older than 15 seconds discard
            if( abs( it.value().lastActualReport.secsTo( now ) ) > 30.0 )
            {
                g_trafficMap.remove( it.key() );
                bTrafficRemoved = true;
                break;
            }
        }
    }
*/
}


// Handle various screen presses (pressing the screen is handled the same as a mouse click here)
void AHRSCanvas::mouseReleaseEvent( QMouseEvent *pEvent )
{
    if( pEvent == 0 )
        return;

    QPoint          pt( pEvent->pos() );
    CanvasConstants c = m_pCanvas->constants();

    // Any x delta greater than half the width of the screen and a continuous mouse move of at least 10 events, is a swipe left or right
    if( ((pt.x() - m_SwipeStart.x()) < -c.dW7) && (m_iSwiping > 5) && (abs( pt.y() - m_SwipeStart.y() ) < c.dH4) )
    {
        m_SwipeStart = QPoint( 0, 0 );
        m_iSwiping = 0;
        swipeLeft();
        return;
    }
    else if( ((pt.x() - m_SwipeStart.x()) > c.dW7) && (m_iSwiping > 5) && (abs( pt.y() - m_SwipeStart.y() ) < c.dH4) )
    {
        m_SwipeStart = QPoint( 0, 0 );
        m_iSwiping = 0;
        swipeRight();
        return;
    }
    // Any y delta greater than 1/2 the screen height (or 1/4 in portrait) and a continuous mouse move of at least 10 events, is a swipe up or down
    else if( ((pt.y() - m_SwipeStart.y()) < -c.dH8) && (m_iSwiping > 5) )
    {
        m_SwipeStart = QPoint( 0, 0 );
        m_iSwiping = 0;
        swipeUp();
        return;
    }
    else if( ((pt.y() - m_SwipeStart.y()) > c.dH8) && (m_iSwiping > 5) )
    {
        m_SwipeStart = QPoint( 0, 0 );
        m_iSwiping = 0;
        swipeDown();
        return;
    }

    AHRSMainWin *pMainWin = static_cast<AHRSMainWin *>( parentWidget()->parentWidget() );
    QDateTime    qdtNow = QDateTime::currentDateTime();

    if( pMainWin->menuActive() )
    {
        pMainWin->menu();
        return;
    }

    if( m_bShowGPSDetails )
    {
        m_bShowGPSDetails = false;
        update();
        return;
    }

    if( m_bDisplayTanksSwitchNotice )
    {
        m_bDisplayTanksSwitchNotice = false;
        m_tanks.bOnLeftTank = (!m_tanks.bOnLeftTank);
        m_tanks.lastSwitch = qdtNow;
        update();
        return;
    }

    if( m_bLongPress && (m_longPressStart.msecsTo( qdtNow ) > 500) )
    {
        m_bShowCrosswind = (!m_bShowCrosswind);
        m_bLongPress = false;
        update();
    }
    else
    {
        m_bLongPress = false;
        handleScreenPress( pt );

        m_bUpdated = true;
        update();
    }
}


void AHRSCanvas::handleScreenPress( const QPoint &pressPt )
{
    CanvasConstants c = m_pCanvas->constants();
    QRect           headRect( (m_bPortrait ? c.dW2 : c.dW + c.dW2) - c.dW4, c.dH - 10.0 - c.dW2 - c.dW4, c.dW2, c.dW2 );
    QRect           gpsRect( (m_bPortrait ? c.dW : c.dWa) - c.dW5, c.dH - (c.iLargeFontHeight * 2.0), c.dW5, c.iLargeFontHeight * 2.0 );
    QRect           directRect;
    QRect           fromtoRect;

    if( m_bPortrait )
    {
        directRect.setRect( c.dW40, c.dH - c.dH20 - c.dH40, c.dH20, c.dH20 );
        fromtoRect.setRect( c.dW40 + c.dH20, c.dH - c.dH20 - c.dH40, c.dH20, c.dH20 );
    }
    else
    {
        directRect.setRect( c.dW + c.dW40, c.dH - c.dH20 - c.dH40, c.dH20, c.dH20 );
        fromtoRect.setRect( c.dW + c.dW40 + c.dH20, c.dH - c.dH20 - c.dH40, c.dH20, c.dH20 );
    }

    QRectF altRect;

    if( m_bPortrait )
        altRect.setRect( c.dW - c.dW5, 0.0, c.dW5, c.dH2 - 40.0 );
    else
        altRect.setRect( c.dW - c.dW5 - c.dW40, 0.0, c.dW5 + c.dW40, c.dH );

    if( altRect.contains( pressPt ) )
    {
        Keypad keypad( this, "ALTITUDE BUG" );

        m_pCanvas->setKeypadGeometry( &keypad );
        if( keypad.exec() == QDialog::Accepted )
            m_iAltBug = static_cast<int>( keypad.value() );
        else
            m_iAltBug = -1;
        update();
    }
    // User pressed the GPS Lat/long area. This needs to be before the test for the heading indicator since it's within that area's rectangle
    else if( gpsRect.contains( pressPt ) )
        m_bShowGPSDetails = (!m_bShowGPSDetails);
    // User pressed on the heading indicator
    else if( headRect.contains( pressPt ) )
    {
        int         iButton = -1;
        BugSelector bugSel( this );

        if( m_bPortrait )
            bugSel.setGeometry( c.dW2 - c.dW4, c.dH2 - c.dH4 - c.dH8, c.dW2, c.dH2 + c.dH4 );
        else
            bugSel.setGeometry( c.dW + c.dW2 - c.dW4, c.dH20, c.dW2, c.dH - c.dH10 );

        iButton = bugSel.exec();

        // Cancelled (do nothing)
        if( iButton == QDialog::Rejected )
            return;
        // Airport details requested
        else if( iButton == static_cast<int>( BugSelector::Airports ) )
        {
            g_bNoAirportsUpdate = true;
            g_apt.waitForFinished();

            AirportDialog airportDlg( this, &c, "SELECT AIRPORT" );

            airportDlg.setGeometry( 0, 0, c.dW, c.dH );
            if( airportDlg.exec() != QDialog::Rejected )
            {
                Airport ap;
                QString qsName = airportDlg.selectedAirport();
                bool    bFound = false;

                foreach( ap, g_airportCache )
                {
                    if( ap.qsName == qsName )
                    {
                        bFound = true;
                        break;
                    }
                }

                if( bFound )
                {
                    DetailsDialog detailsDlg( this, &c, &ap );

                    detailsDlg.setGeometry( 0, 0, c.dW, c.dH );

                    detailsDlg.exec();
                    g_bNoAirportsUpdate = false;
                    return;
                }
            }
            g_bNoAirportsUpdate = false;
        }
        // Bugs cleared - reset both to invalid and get out
        else if( iButton == static_cast<int>( BugSelector::ClearBugs ) )
        {
            m_iHeadBugAngle = -1;
            m_iWindBugAngle = -1;
            return;
        }
        else if( iButton == static_cast<int>( BugSelector::Overlays ) )
        {
            Overlays overlaysDlg( this );

            if( m_bPortrait )
                overlaysDlg.setGeometry( c.dW2 - c.dW4, c.dH2 - c.dH8, c.dW2, c.dH2 + c.dH8 - c.dH20 );
            else
                overlaysDlg.setGeometry( c.dW + c.dW2 - c.dW4, c.dH2 - c.dH4 - c.dH20, c.dW2, c.dH2 + c.dH4 );

            connect( &overlaysDlg, SIGNAL( trafficToggled( bool ) ), this, SLOT( showAllTraffic( bool ) ) );
            connect( &overlaysDlg, SIGNAL( showAirports( Canvas::ShowAirports ) ), this, SLOT( showAirports( Canvas::ShowAirports ) ) );
            connect( &overlaysDlg, SIGNAL( showRunways( bool ) ), this, SLOT( showRunways( bool ) ) );
            connect( &overlaysDlg, SIGNAL( showAirspaces( bool ) ), this, SLOT( showAirspaces( bool ) ) );
            connect( &overlaysDlg, SIGNAL( showAltitudes( bool ) ), this, SLOT( showAltitudes( bool ) ) );

            overlaysDlg.exec();
            return;
        }
        else if( iButton == static_cast<int>( BugSelector::BaroPress ) )
        {
            Keypad baro( this, "BARO PRESS" );

            m_pCanvas->setKeypadGeometry( &baro );
            if( baro.exec() == QDialog::Accepted )
            {
                m_dBaroPress = baro.value();
                static_cast<AHRSMainWin *>( parentWidget()->parentWidget() )->streamReader()->setBaroPress( m_dBaroPress );
            }
            return;
        }

        Keypad keypad( this, "HEADING" );

        m_pCanvas->setKeypadGeometry( &keypad );

        if( iButton == static_cast<int>( BugSelector::WindBug ) )
            keypad.setTitle( "WIND FROM HEADING" );

        if( keypad.exec() == QDialog::Accepted )
        {
            int iAngle = static_cast<int>( keypad.value() );

            // Automatically wrap around
            while( iAngle > 360 )
                iAngle -= 360;
            // Heading bug
            if( iButton == static_cast<int>( BugSelector::HeadingBug ) )
                m_iHeadBugAngle = iAngle;
            // Wind bug
            else if( iButton == static_cast<int>( BugSelector::WindBug ) )
            {
                m_iWindBugAngle = iAngle;
                keypad.clear();
                keypad.setTitle( "WIND SPEED" );
                keypad.exec();
                m_iWindBugSpeed = keypad.value();
            }
        }
        else
        {   // Heading bug
            if( iButton == QDialog::Accepted )
                m_iHeadBugAngle = -1;
            // Wind bug
            else if( iButton == QDialog::Rejected )
                m_iWindBugAngle = -1;
        }
    }
    else if( directRect.contains( pressPt ) )
    {
        g_bNoAirportsUpdate = true;
        g_apt.waitForFinished();

        AirportDialog dlg( this, &c, "DIRECT TO AIRPORT" );

        // This geometry works for both orientations
        dlg.setGeometry( 0, 0, c.dW, c.dH );
        // If the dialog wasn't cancelled then find the lat/long of the airport selected
        if( dlg.exec() != QDialog::Rejected )
        {
            Airport ap;
            QString qsName = dlg.selectedAirport();

            foreach( ap, g_airportCache )
            {
                if( ap.qsName == qsName )
                {
                    m_directAP = ap;
                    break;
                }
            }
            // Invalidate from-to in favor of direct-to
            m_fromAP.qsID = "NULL";
            m_fromAP.qsName = "NULL";
            m_toAP.qsID = "NULL";
            m_toAP.qsName = "NULL";
        }
        else
        {
            m_directAP.qsID = "NULL";
            m_directAP.qsName = "NULL";
        }

        g_bNoAirportsUpdate = false;
    }
    else if( fromtoRect.contains( pressPt ) )
    {
        g_bNoAirportsUpdate = true;
        g_apt.waitForFinished();

        AirportDialog dlgFrom( this, &c, "FROM AIRPORT" );

        // This geometry works for both orientations
        dlgFrom.setGeometry( 0, 0, c.dW, c.dH );
        // If the dialog wasn't cancelled then find the lat/long of the airport selected
        if( dlgFrom.exec() != QDialog::Rejected )
        {
            Airport ap;
            QString qsName = dlgFrom.selectedAirport();

            foreach( ap, g_airportCache )
            {
                if( ap.qsName == qsName )
                {
                    m_fromAP = ap;
                    break;
                }
            }

            AirportDialog dlgTo( this, &c, "TO AIRPORT" );

            dlgTo.setGeometry( 0, 0, c.dW, c.dH );
            if( dlgTo.exec() != QDialog::Rejected )
            {
                qsName = dlgTo.selectedAirport();

                foreach( ap, g_airportCache )
                {
                    if( ap.qsName == qsName )
                    {
                        m_directAP.qsID = "NULL";
                        m_directAP.qsName = "NULL";
                        m_toAP = ap;
                        break;
                    }
                }
            }
            // Invalidate direct-to in favor of from-to
            else
            {
                m_directAP.qsID = "NULL";
                m_directAP.qsName = "NULL";
                m_fromAP.qsID = "NULL";
                m_fromAP.qsName = "NULL";
                m_toAP.qsID = "NULL";
                m_toAP.qsName = "NULL";
            }
        }
        else
        {
            m_directAP.qsID = "NULL";
            m_directAP.qsName = "NULL";
            m_fromAP.qsID = "NULL";
            m_fromAP.qsName = "NULL";
            m_toAP.qsID = "NULL";
            m_toAP.qsName = "NULL";
        }

        g_bNoAirportsUpdate = false;
    }
}


void AHRSCanvas::mousePressEvent( QMouseEvent *pEvent )
{
    CanvasConstants c = m_pCanvas->constants();
    QRect           headRect;

    if( m_bPortrait )
        headRect = QRect( c.dW2 - c.dW4, c.dH2, c.dW2, c.dH2 - c.dH10 );
    else
        headRect = QRect( c.dW + c.dW2 - c.dW4, c.dH2 - c.dH4, c.dW2, c.dH2 );

    m_SwipeStart = pEvent->pos();
    m_iSwiping = 0;

    if( headRect.contains( pEvent->pos() ) )
    {
        m_longPressStart = QDateTime::currentDateTime();
        m_bLongPress = true;
    }
}


void AHRSCanvas::mouseMoveEvent( QMouseEvent *pEvent )
{
    Q_UNUSED( pEvent )

    m_iSwiping++;
}


void AHRSCanvas::zoomIn()
{
    m_dZoomNM -= 5.0;
    if( m_dZoomNM < 5.0 )
        m_dZoomNM = 5.0;
    g_pSet->setValue( "ZoomNM", m_dZoomNM );
    g_pSet->sync();
    QtConcurrent::run( TrafficMath::updateNearbyAirports, &m_airports, &m_directAP, &m_fromAP, &m_toAP, m_dZoomNM );
}


void AHRSCanvas::zoomOut()
{
    m_dZoomNM += 5.0;
    if( m_dZoomNM > 100.0 )
        m_dZoomNM = 100.0;
    g_pSet->setValue( "ZoomNM", m_dZoomNM );
    g_pSet->sync();
    QtConcurrent::run( TrafficMath::updateNearbyAirports, &m_airports, &m_directAP, &m_fromAP, &m_toAP, m_dZoomNM );
}


void AHRSCanvas::showAllTraffic( bool bAll )
{
    m_settings.bShowAllTraffic = bAll;
    g_pSet->setValue( "ShowAllTraffic", bAll );
    g_pSet->sync();
    update();
}


void AHRSCanvas::showAirports( Canvas::ShowAirports eShow )
{
    m_settings.eShowAirports = eShow;
    g_pSet->setValue( "ShowAirports", static_cast<int>( eShow ) );
    g_pSet->sync();
    update();
}


void AHRSCanvas::showRunways( bool bShow )
{
    m_settings.bShowRunways = bShow;
    g_pSet->setValue( "ShowRunways", bShow );
    g_pSet->sync();
    update();
}


void AHRSCanvas::showAirspaces( bool bShow )
{
    m_settings.bShowAirspaces = bShow;
    g_pSet->setValue( "ShowAirspaces", bShow );
    g_pSet->sync();
    update();
}


void AHRSCanvas::showAltitudes( bool bShow )
{
    m_settings.bShowAltitudes = bShow;
    g_pSet->setValue( "ShowAltitudes", bShow );
    g_pSet->sync();
    update();
}


void AHRSCanvas::paintPortrait()
{
    QPainter        ahrs( this );
    CanvasConstants c = m_pCanvas->constants();
    QPixmap         num( 320, 84 );
    QPolygon        shape;
    QPen            linePen( Qt::black );
    double          dPitchH = c.dH4 + (g_situation.dAHRSpitch / 22.5 * c.dH4);     // The visible portion is only 1/4 of the 90 deg range
    double          dSlipSkid = c.dW2 - ((g_situation.dAHRSSlipSkid / 4.0) * c.dW2);
    double          dPxPerVSpeed = c.dH2 / 40.0;
    double          dPxPerFt = static_cast<double>( m_AltTape.height() ) / 20000.0 * 0.99;
    double          dPxPerKnot = static_cast<double>( m_SpeedTape.height() ) / 300.0 * 0.99;

    if( dSlipSkid < (c.dW4 + 25.0) )
        dSlipSkid = c.dW4 + 25.0;
    else if( dSlipSkid > (c.dW2 + c.dW4 - 25.0) )
        dSlipSkid = c.dW2 + c.dW4 - 25.0;

    linePen.setWidth( c.iThinPen );

    // Don't draw past the bottom of the fuel indicators
    ahrs.setClipRect( 0, 0, c.dW, c.dH2 + c.dH5 );

    ahrs.setRenderHints( QPainter::Antialiasing | QPainter::TextAntialiasing, true );

    // Translate to dead center and rotate by stratux roll then translate back
    ahrs.translate( c.dW2, c.dH4 );
    ahrs.rotate( -g_situation.dAHRSroll );
    ahrs.translate( -c.dW2, -c.dH4 );

    // Top half sky blue gradient offset by stratux pitch
    QLinearGradient skyGradient( 0.0, -c.dH2, 0.0, dPitchH );
    skyGradient.setColorAt( 0, Qt::blue );
    skyGradient.setColorAt( 1, QColor( 85, 170, 255 ) );
    ahrs.fillRect( -400.0, -c.dH4, c.dW + 800.0, dPitchH + c.dH4, skyGradient );

    // Draw brown gradient horizon half offset by stratux pitch
    // Extreme overdraw accounts for extreme roll angles that might expose the corners
    QLinearGradient groundGradient( 0.0, dPitchH, 0, c.dH2 );
    groundGradient.setColorAt( 0, QColor( 170, 85, 0  ) );
    groundGradient.setColorAt( 1, Qt::black );

    ahrs.fillRect( -400.0, dPitchH, c.dW + 800.0, c.dH4 + c.dH5, groundGradient );
    ahrs.setPen( linePen );
    ahrs.drawLine( -400, dPitchH, c.dW + 800.0, dPitchH );

    for( int i = 0; i < 20; i += 10 )
    {
        linePen.setColor( Qt::cyan );
        ahrs.setPen( linePen );
        ahrs.drawLine( c.dW2 - c.dW20, dPitchH - ((i + 2.5) / 45.0 * c.dH4), c.dW2 + c.dW20, dPitchH - ((i + 2.5) / 45.0 * c.dH4) );
        ahrs.drawLine( c.dW2 - c.dW20, dPitchH - ((i + 5.0) / 45.0 * c.dH4), c.dW2 + c.dW20, dPitchH - ((i + 5.0) / 45.0 * c.dH4) );
        ahrs.drawLine( c.dW2 - c.dW20, dPitchH - ((i + 7.5) / 45.0 * c.dH4), c.dW2 + c.dW20, dPitchH - ((i + 7.5) / 45.0 * c.dH4) );
        ahrs.drawLine( c.dW2 - c.dW5, dPitchH - ((i + 10.0) / 45.0 * c.dH4), c.dW2 + c.dW5, dPitchH - (( i + 10.0) / 45.0 * c.dH4) );
        linePen.setColor( QColor( 67, 33, 9 ) );
        ahrs.setPen( linePen );
        ahrs.drawLine( c.dW2 - c.dW20, dPitchH + ((i + 2.5) / 45.0 * c.dH4), c.dW2 + c.dW20, dPitchH + ((i + 2.5) / 45.0 * c.dH4) );
        ahrs.drawLine( c.dW2 - c.dW20, dPitchH + ((i + 5.0) / 45.0 * c.dH4), c.dW2 + c.dW20, dPitchH + ((i + 5.0) / 45.0 * c.dH4) );
        ahrs.drawLine( c.dW2 - c.dW20, dPitchH + ((i + 7.5) / 45.0 * c.dH4), c.dW2 + c.dW20, dPitchH + ((i + 7.5) / 45.0 * c.dH4) );
        ahrs.drawLine( c.dW2 - c.dW5, dPitchH + ((i + 10.0) / 45.0 * c.dH4), c.dW2 + c.dW5, dPitchH + (( i + 10.0) / 45.0 * c.dH4) );
    }

    // Reset rotation and clipping
    ahrs.resetTransform();
    ahrs.setClipping( false );

    // Slip/Skid indicator
    drawSlipSkid( &ahrs, &c, dSlipSkid );

    // Draw the top roll indicator
    ahrs.translate( c.dW2, c.dH20 + ((c.dW - c.dW5) / 2.0) );
    ahrs.rotate( -g_situation.dAHRSroll );
    ahrs.translate( -c.dW2, -(c.dH20 + ((c.dW - c.dW5) / 2.0)) );
    ahrs.drawPixmap( c.dW10, c.dH20, c.dW - c.dW5, c.dW - c.dW5, m_RollIndicator );
    ahrs.resetTransform();

    QPolygonF arrow;

    arrow.append( QPointF( c.dW2, c.dH40 + (c.dH * 0.0625) ) );
    arrow.append( QPointF( c.dW2 + (c.dWa * 0.03125), c.dH40 + (c.dH * 0.08125) ) );
    arrow.append( QPointF( c.dW2 - (c.dWa * 0.03125), c.dH40 + (c.dH * 0.08125) ) );
    ahrs.setBrush( Qt::white );
    ahrs.setPen( Qt::black );
    ahrs.drawPolygon( arrow );

    // Draw the yellow pitch indicators
    ahrs.setBrush( Qt::yellow );
    shape.append( QPoint( c.dW5 + c.dW20, c.dH4 - c.dH160 ) );
    shape.append( QPoint( c.dW2 - c.dW10, c.dH4 - c.dH160 ) );
    shape.append( QPoint( c.dW2 - c.dW10 + 20, c.dH4 ) );
    shape.append( QPoint( c.dW2 - c.dW10, c.dH4 + c.dH160 ) );
    shape.append( QPoint( c.dW5 + c.dW20, c.dH4 + c.dH160 ) );
    ahrs.drawPolygon( shape );
    shape.clear();
    shape.append( QPoint( c.dW - c.dW5 - c.dW20, c.dH4 - c.dH160 ) );
    shape.append( QPoint( c.dW2 + c.dW10, c.dH4 - c.dH160 ) );
    shape.append( QPoint( c.dW2 + c.dW10 - 20, c.dH4 ) );
    shape.append( QPoint( c.dW2 + c.dW10, c.dH4 + c.dH160 ) );
    shape.append( QPoint( c.dW - c.dW5 - c.dW20, c.dH4 + c.dH160 ) );
    ahrs.drawPolygon( shape );
    shape.clear();
    shape.append( QPoint( c.dW2, c.dH4 ) );
    shape.append( QPoint( c.dW2 - c.dW20, c.dH4 + 20 ) );
    shape.append( QPoint( c.dW2 + c.dW20, c.dH4 + 20 ) );
    ahrs.drawPolygon( shape );

    // Draw the Altitude tape
    QPainterPath maskPath, elipsePath;

    maskPath.addRect( 0.0, 0.0, c.dW, c.dH );
    elipsePath.addEllipse( m_bPortrait ? 0.0 : c.dW,
                           c.dH - c.dW,
                           c.dW, c.dW );
    maskPath = maskPath.subtracted( elipsePath );
    ahrs.setClipPath( maskPath );

    ahrs.fillRect( c.dW - c.dW5, 0, c.dW5, c.dH2 + c.dH4, QColor( 0, 0, 0, 100 ) );
    ahrs.drawPixmap( c.dW - c.dW5 + 5, c.dH4 + 10.0 - m_AltTape.height() + (g_situation.dBaroPressAlt * dPxPerFt), m_AltTape );

    // Draw the Speed tape
    ahrs.fillRect( 0, 0, c.dW10 + 5.0, c.dH2, QColor( 0, 0, 0, 100 ) );
    ahrs.setClipRect( 2.0, 2.0, c.dW5 - 4.0, c.dH2 + c.dH4 );
    if( g_situation.bHaveWTData )
        ahrs.drawPixmap( 5, c.dH4 + 5.0 - m_SpeedTape.height() + (g_situation.dTAS * dPxPerKnot), m_SpeedTape );
    else
        ahrs.drawPixmap( 5, c.dH4 + 5.0 - m_SpeedTape.height() + (g_situation.dGPSGroundSpeed * dPxPerKnot), m_SpeedTape );
    ahrs.setClipping( false );

    // Draw the current speed
    if( g_situation.bHaveWTData )
    {
        Builder::buildNumber( &num, &c, static_cast<int>( g_situation.dTAS ), 0 );
        drawCurrSpeed( &ahrs, &c, &num );
        Builder::buildNumber( &num, &c, static_cast<int>( g_situation.dGPSGroundSpeed ), 0 );

        // Draw the ground speed just below the indicator since we have both, and both are useful
        drawCurrSpeed( &ahrs, &c, &num, true );
    }
    else
    {
        Builder::buildNumber( &num, &c, static_cast<int>( g_situation.dGPSGroundSpeed ), 0 );
        drawCurrSpeed( &ahrs, &c, &num );
    }

    ahrs.setFont( wee );
    ahrs.setPen( Qt::black );
    QString qsUnits( speedUnits() );
    ahrs.drawText( c.dW10 + c.dW40 + (c.dW80 / 2.0) + 1, c.dH4 + 1, qsUnits );
    ahrs.setPen( Qt::white );
    ahrs.drawText( c.dW10 + c.dW40 + (c.dW80 / 2.0), c.dH4, qsUnits );

    ahrs.setClipping( false );

    // Left Tank indicators background
    ahrs.drawPixmap( 0.0, c.dH2 + c.dH40, c.dW20, c.dH2 - c.dH5, m_Lfuel );
    // Tank indicators level
    QPen levelPen( Qt::black, c.dH40 + 4 );
    levelPen.setCapStyle( Qt::RoundCap );
    ahrs.setPen( levelPen );
    ahrs.drawLine( 0.0,
                   c.dH2 + c.dH40 + ((c.dH2 - c.dH5) * ((m_tanks.dLeftCapacity - m_tanks.dLeftRemaining) / m_tanks.dLeftCapacity)),
                   c.dW40,
                   c.dH2 + c.dH40 + ((c.dH2 - c.dH5) * ((m_tanks.dLeftCapacity - m_tanks.dLeftRemaining) / m_tanks.dLeftCapacity)) );
    levelPen.setWidth( c.dH40 );
    levelPen.setColor( QColor( 255, 150, 255 ) );
    ahrs.setPen( levelPen );
    ahrs.drawLine( 0.0,
                   c.dH2 + c.dH40 + ((c.dH2 - c.dH5) * ((m_tanks.dLeftCapacity - m_tanks.dLeftRemaining) / m_tanks.dLeftCapacity)),
                   c.dW40,
                   c.dH2 + c.dH40 + ((c.dH2 - c.dH5) * ((m_tanks.dLeftCapacity - m_tanks.dLeftRemaining) / m_tanks.dLeftCapacity)) );

    if( m_tanks.bDualTanks )
    {
        // Right Tank indicators background
        ahrs.drawPixmap( c.dW - c.dW20 - 1, c.dH2 + c.dH40, c.dW20, c.dH2 - c.dH5, m_Rfuel );
        // Right Tank indicators level
        levelPen.setColor( Qt::black );
        levelPen.setWidth( c.dH40 + 4 );
        ahrs.setPen( levelPen );
        ahrs.drawLine( c.dW,
                       c.dH2 + c.dH40 + ((c.dH2 - c.dH5) * ((m_tanks.dRightCapacity - m_tanks.dRightRemaining) / m_tanks.dRightCapacity)),
                       c.dW - c.dW40 - 1,
                       c.dH2 + c.dH40 + ((c.dH2 - c.dH5) * ((m_tanks.dRightCapacity - m_tanks.dRightRemaining) / m_tanks.dRightCapacity)) );
        levelPen.setWidth( c.dH40 );
        levelPen.setColor( QColor( 255, 150, 255 ) );
        ahrs.setPen( levelPen );
        ahrs.drawLine( c.dW,
                       c.dH2 + c.dH40 + ((c.dH2 - c.dH5) * ((m_tanks.dRightCapacity - m_tanks.dRightRemaining) / m_tanks.dRightCapacity)),
                       c.dW - c.dW40 - 1,
                       c.dH2 + c.dH40 + ((c.dH2 - c.dH5) * ((m_tanks.dRightCapacity - m_tanks.dRightRemaining) / m_tanks.dRightCapacity)) );
    }

    // Tank indicator active indicators
    ahrs.setFont( large );
    if( m_bFuelFlowStarted )
    {
        QPen fuelPen( Qt::yellow, c.dH80 );

        ahrs.setPen( fuelPen );

        if( m_tanks.bOnLeftTank || (!m_tanks.bDualTanks) )
            ahrs.drawLine( 0, c.dH2 + c.dH40 - 15, c.dW10 - 2, c.dH2 + c.dH40 - 15 );
        else
            ahrs.drawLine( c.dW - c.dW10 + 2, c.dH2 + c.dH40 - 15, c.dW, c.dH2 + c.dH40 - 15 );
    }

    // Arrow for heading position above heading dial
    arrow.clear();
    arrow.append( QPointF( c.dW2, c.dH - c.dW - 10.0 - c.dH80 ) );
    arrow.append( QPointF( c.dW2 + c.dW40, c.dH - c.dW - 10.0 - c.dH40 ) );
    arrow.append( QPointF( c.dW2 - c.dW40, c.dH - c.dW - 10.0 - c.dH40 ) );
    ahrs.setBrush( Qt::white );
    ahrs.setPen( Qt::black );
    ahrs.drawPolygon( arrow );

    // Draw the heading value over the indicator
    ahrs.setPen( QPen( Qt::white, c.iThinPen ) );
    ahrs.setBrush( Qt::black );
    ahrs.drawRect( c.dW2 - (c.dWNum * 3.0 / 2.0) - (c.dW * 0.0125), arrow.boundingRect().y() - c.dHNum - c.dH40 - (c.dH * 0.0075), (c.dWNum * 3.0) + (c.dW * 0.025), c.dHNum + (c.dH * 0.015) );
    if( g_situation.bHaveWTData )
        Builder::buildNumber( &num, &c, static_cast<int>( g_situation.dAHRSMagHeading ), 3 );
    else
        Builder::buildNumber( &num, &c, static_cast<int>( g_situation.dAHRSGyroHeading ), 3 );
    ahrs.drawPixmap( c.dW2 - (c.dWNum * 3.0 / 2.0), arrow.boundingRect().y() - c.dHNum - c.dH40, num );

    // Draw the heading pixmap and rotate it to the current heading
    ahrs.translate( c.dW2, c.dH - c.dW2 - 20.0 );
    if( g_situation.bHaveWTData )
        ahrs.rotate( -g_situation.dAHRSMagHeading );
    else
        ahrs.rotate( -g_situation.dAHRSGyroHeading );
    ahrs.translate( -c.dW2, -(c.dH - c.dW2 - 20.0) );
    ahrs.drawPixmap( 10, c.dH - c.dW - 10.0, c.dW - 20, c.dW - 20,  m_HeadIndicator );
    ahrs.resetTransform();

    drawDirectOrFromTo( &ahrs, &c );

    // Draw the central airplane
    ahrs.drawPixmap( c.dW2 - c.dW20, c.dH - 20.0 - c.dW2 - c.dW20, c.dW10, c.dW10, m_planeIcon );

    // Draw the heading bug
    if( m_iHeadBugAngle >= 0 )
    {
        ahrs.translate( c.dW2, c.dH - 20.0 - c.dW2 );
        if( g_situation.bHaveWTData )
            ahrs.rotate( m_iHeadBugAngle - g_situation.dAHRSMagHeading );
        else
            ahrs.rotate( m_iHeadBugAngle - g_situation.dAHRSGyroHeading );
        ahrs.translate( -c.dW2, -(c.dH - 20.0 - c.dW2) );
        ahrs.drawPixmap( c.dW2 - (m_headIcon.width() / 2), c.dH - c.dH10 - c.dW + (m_headIcon.height() / 2), m_headIcon );

        // If long press triggered crosswind component display and the wind bug is set
        if( m_bShowCrosswind && (m_iWindBugAngle >= 0) )
        {
            linePen.setWidth( c.iThinPen );
            linePen.setColor( QColor( 0xFF, 0x90, 0x01 ) );
            ahrs.setPen( linePen );
            ahrs.drawLine( c.dW2, c.dH - c.dW - 10.0, c.dW2, c.dH - 10.0 - c.dW2 );
        }

        ahrs.resetTransform();
    }

    // Draw the wind bug
    if( m_iWindBugAngle >= 0 )
    {
        ahrs.translate( c.dW2, c.dH - c.dW2 - 20.0 );
        if( g_situation.bHaveWTData )
            ahrs.rotate( m_iWindBugAngle - g_situation.dAHRSMagHeading );
        else
            ahrs.rotate( m_iWindBugAngle - g_situation.dAHRSGyroHeading );
        ahrs.translate( -c.dW2, -(c.dH - c.dW2 - 20.0) );
        ahrs.drawPixmap( c.dW2 - (m_headIcon.width() / 2), c.dH - c.dH10 - c.dW + (m_headIcon.height() / 2), m_windIcon );

        QString      qsWind = QString::number( m_iWindBugSpeed );
        QFontMetrics windMetrics( med );
        QRect        windRect = windMetrics.boundingRect( qsWind );

        ahrs.setFont( med );
        ahrs.setPen( Qt::black );
        ahrs.drawText( c.dW2 - (windRect.width() / 2), c.dH - c.dW - c.dH160, qsWind );
        ahrs.setPen( Qt::white );
        ahrs.drawText( c.dW2 - (windRect.width() / 2) - 1, c.dH - c.dW - c.dH160 - 1, qsWind );

        // If long press triggered crosswind component display and the heading bug is set
        if( m_bShowCrosswind && (m_iHeadBugAngle >= 0) )
        {
            linePen.setWidth( c.iThinPen );
            linePen.setColor( Qt::cyan );
            ahrs.setPen( linePen );
            ahrs.drawLine( c.dW2, c.dH - c.dW - 10, c.dW2, c.dH - 10.0 - c.dW2 );

            // Draw the crosswind component calculated from heading vs wind
            double dAng = fabs( static_cast<double>( m_iWindBugAngle ) - static_cast<double>( m_iHeadBugAngle ) );
            while( dAng > 180.0 )
                dAng -= 360.0;
            dAng = fabs( dAng );
            double dCrossComp = fabs( static_cast<double>( m_iWindBugSpeed ) * sin( dAng * ToRad ) );
            double dCrossPos = c.dH - (c.dW / 1.3) - 10.0;
            QString qsCrossAng = QString( "%1%2" ).arg( static_cast<int>( dAng ) ).arg( QChar( 176 ) );

            ahrs.resetTransform();
            ahrs.translate( c.dW2, c.dH - c.dW2 - 10.0 );
            if( g_situation.bHaveWTData )
                ahrs.rotate( m_iHeadBugAngle - g_situation.dAHRSMagHeading );
            else
                ahrs.rotate( m_iHeadBugAngle - g_situation.dAHRSGyroHeading );
            ahrs.translate( -c.dW2, -(c.dH - c.dW2 - 10.0) );
            ahrs.setFont( large );
            ahrs.setPen( Qt::black );
            ahrs.drawText( c.dW2 + 5.0, dCrossPos, QString::number( static_cast<int>( dCrossComp ) ) );
            ahrs.setPen( QColor( 0xFF, 0x90, 0x01 ) );
            ahrs.drawText( c.dW2 + 3.0, dCrossPos - 2.0, QString::number( static_cast<int>( dCrossComp ) ) );
            ahrs.setFont( med );
            ahrs.setPen( Qt::black );
            ahrs.drawText( c.dW2 + 5.0, dCrossPos + c.iMedFontHeight - 5, qsCrossAng );
            ahrs.setPen( Qt::cyan );
            ahrs.drawText( c.dW2 + 3.0, dCrossPos + c.iMedFontHeight - 7, qsCrossAng );
        }

        ahrs.resetTransform();
    }

    // Draw the altitude bug
    if( m_iAltBug >= 0 )
    {
        double dAltTip = c.dH4 - 10.0 - ((static_cast<double>( m_iAltBug ) - g_situation.dBaroPressAlt) * dPxPerFt);

        ahrs.drawPixmap( c.dW - c.dW5 - c.dW20, dAltTip - c.dH40, c.dW20, c.dH20, m_AltBug );
    }

    // Draw the vertical speed static pixmap
    ahrs.fillRect( c.dW - c.dW20 - c.dW40, 0.0, c.dW40, c.dH2, QColor( 0, 0, 0, 100 ) );
    ahrs.drawPixmap( c.dW - c.dW20, 0, c.dW20, c.dH2, m_VertSpeedTape );

    // Draw the vertical speed indicator
    ahrs.translate( 0.0, c.dH4 - (dPxPerVSpeed * g_situation.dGPSVertSpeed / 100.0 * 0.98) );   // 98% accounts for the slight margin on each end
    arrow.clear();
    arrow.append( QPoint( c.dW - m_pCanvas->scaledH( 30.0 ), 0.0 ) );
    arrow.append( QPoint( c.dW - m_pCanvas->scaledH( 20.0 ), m_pCanvas->scaledV( -7.0 ) ) );
    arrow.append( QPoint( c.dW, m_pCanvas->scaledV( -15.0 ) ) );
    arrow.append( QPoint( c.dW, m_pCanvas->scaledV( 15.0 ) ) );
    arrow.append( QPoint( c.dW - m_pCanvas->scaledH( 20.0 ), m_pCanvas->scaledV( 7.0 ) ) );
    ahrs.setPen( Qt::black );
    ahrs.setBrush( Qt::white );
    ahrs.drawPolygon( arrow );

    QString qsFullVspeed = QString::number( g_situation.dGPSVertSpeed / 100.0, 'f', 1 );
    QString qsFracVspeed = qsFullVspeed.right( 1 );
    QString qsIntVspeed = qsFullVspeed.left( qsFullVspeed.length() - 2 );
    QFontMetrics weeMetrics( wee );

    // Draw vertical speed indicator as In thousands and hundreds of FPM in tiny text on the vertical speed arrow
    ahrs.setFont( wee );
    QRect intRect( weeMetrics.boundingRect( qsIntVspeed ) );
    ahrs.drawText( c.dW - m_pCanvas->scaledH( 20.0 ), m_pCanvas->scaledV( 4.0 ), qsIntVspeed );
    ahrs.setFont( itsy );
    ahrs.drawText( c.dW - m_pCanvas->scaledH( 20.0 ) + intRect.width() + 2, m_pCanvas->scaledV( 4.0 ), qsFracVspeed );
    ahrs.resetTransform();

    // Draw the current altitude
    Builder::buildNumber( &num, &c, static_cast<int>( g_situation.dBaroPressAlt ), 0 );
    drawCurrAlt( &ahrs, &c, &num );

    // Draw the G-Force indicator scale
    ahrs.setFont( tiny );
    ahrs.setPen( Qt::black );
    ahrs.drawText( c.dW - c.dW5, c.dH - 15, "0" );
    ahrs.drawText( c.dW - c.dW5 + c.dW10 - (c.iTinyFontWidth / 2), c.dH - 15, "1" );
    ahrs.drawText( c.dW - c.iTinyFontWidth - 10, c.dH - 15, "2" );
    ahrs.setPen( Qt::white );
    ahrs.drawText( c.dW - c.dW5 + 1, c.dH - 16, "0" );
    ahrs.drawText( c.dW - c.dW5 + c.dW10 - (c.iTinyFontWidth / 2), c.dH - 16, "1" );
    ahrs.drawText( c.dW - c.iTinyFontWidth - 11, c.dH - 16, "2" );

    // Arrow for G-Force indicator
    arrow.clear();
    arrow.append( QPoint( 1, c.dH - c.iTinyFontHeight - m_pCanvas->scaledV( 10.0 ) ) );
    arrow.append( QPoint( m_pCanvas->scaledH( -14.0 ), c.dH - c.iTinyFontHeight - m_pCanvas->scaledV( 25.0 ) ) );
    arrow.append( QPoint( m_pCanvas->scaledH( 16.0 ), c.dH - c.iTinyFontHeight - m_pCanvas->scaledV( 25.0 ) ) );
    ahrs.setPen( Qt::black );
    ahrs.setBrush( Qt::white );
    ahrs.translate( c.dW - c.dW5 + (c.iTinyFontWidth / 2) + (fabs( 1.0 - g_situation.dAHRSGLoad ) * c.dW5 * 20.0), -c.dH160 );
    ahrs.drawPolygon( arrow );
    ahrs.resetTransform();

    // Update the airspace positions
    updateAirspaces( &ahrs, &c );

    // Update the airport positions
    if( m_settings.eShowAirports != Canvas::ShowNoAirports )
        updateAirports( &ahrs, &c );

    // Update the traffic positions
    updateTraffic( &ahrs, &c );

    ahrs.drawPixmap( c.dW40, c.dH - c.dH20 - c.dH40, c.dH20, c.dH20, m_DirectTo );
    ahrs.drawPixmap( c.dW40 + c.dH20, c.dH - c.dH20 - c.dH40, c.dH20, c.dH20, m_FromTo );

    // Draw the transparent overlay over the existing heading so the ticks and heading numbers are always visible
    ahrs.translate( c.dW2, c.dH - c.dW2 - 20.0 );
    if( g_situation.bHaveWTData )
        ahrs.rotate( -g_situation.dAHRSMagHeading );
    else
        ahrs.rotate( -g_situation.dAHRSGyroHeading );
    ahrs.translate( -c.dW2, -(c.dH - c.dW2 - 20.0) );
    ahrs.drawPixmap( 10, c.dH - c.dW - 10.0, c.dW - 20, c.dW - 20,  m_HeadIndicatorOverlay );
    ahrs.resetTransform();

    // Draw the heading bug
    if( m_iHeadBugAngle >= 0 )
    {
        ahrs.translate( c.dW2, c.dH - 20.0 - c.dW2 );
        if( g_situation.bHaveWTData )
            ahrs.rotate( m_iHeadBugAngle - g_situation.dAHRSMagHeading );
        else
            ahrs.rotate( m_iHeadBugAngle - g_situation.dAHRSGyroHeading );
        ahrs.translate( -c.dW2, -(c.dH - 20.0 - c.dW2) );
        ahrs.drawPixmap( c.dW2 - (m_headIcon.width() / 2), c.dH - c.dH10 - c.dW + (m_headIcon.height() / 2), m_headIcon );

        // If long press triggered crosswind component display and the wind bug is set
        if( m_bShowCrosswind && (m_iWindBugAngle >= 0) )
        {
            linePen.setWidth( c.iThinPen );
            linePen.setColor( QColor( 0xFF, 0x90, 0x01 ) );
            ahrs.setPen( linePen );
            ahrs.drawLine( c.dW2, c.dH - c.dW - 10.0, c.dW2, c.dH - 10.0 - c.dW2 );
        }

        ahrs.resetTransform();
    }

    // Draw the wind bug
    if( m_iWindBugAngle >= 0 )
    {
        ahrs.translate( c.dW2, c.dH - c.dW2 - 20.0 );
        if( g_situation.bHaveWTData )
            ahrs.rotate( m_iWindBugAngle - g_situation.dAHRSMagHeading );
        else
            ahrs.rotate( m_iWindBugAngle - g_situation.dAHRSGyroHeading );
        ahrs.translate( -c.dW2, -(c.dH - c.dW2 - 20.0) );
        ahrs.drawPixmap( c.dW2 - (m_headIcon.width() / 2), c.dH - c.dH10 - c.dW + (m_headIcon.height() / 2), m_windIcon );

        QString      qsWind = QString::number( m_iWindBugSpeed );
        QFontMetrics windMetrics( med );
        QRect        windRect = windMetrics.boundingRect( qsWind );

        ahrs.setFont( med );
        ahrs.setPen( Qt::black );
        ahrs.drawText( c.dW2 - (windRect.width() / 2), c.dH - c.dW - c.dH160, qsWind );
        ahrs.setPen( Qt::white );
        ahrs.drawText( c.dW2 - (windRect.width() / 2) - 1, c.dH - c.dW - c.dH160 - 1, qsWind );

        // If long press triggered crosswind component display and the heading bug is set
        if( m_bShowCrosswind && (m_iHeadBugAngle >= 0) )
        {
            linePen.setWidth( c.iThinPen );
            linePen.setColor( Qt::cyan );
            ahrs.setPen( linePen );
            ahrs.drawLine( c.dW2, c.dH - c.dW - 10, c.dW2, c.dH - 10.0 - c.dW2 );

            // Draw the crosswind component calculated from heading vs wind
            double dAng = fabs( static_cast<double>( m_iWindBugAngle ) - static_cast<double>( m_iHeadBugAngle ) );
            while( dAng > 180.0 )
                dAng -= 360.0;
            dAng = fabs( dAng );
            double dCrossComp = fabs( static_cast<double>( m_iWindBugSpeed ) * sin( dAng * ToRad ) );
            double dCrossPos = c.dH - (c.dW / 1.3) - 10.0;
            QString qsCrossAng = QString( "%1%2" ).arg( static_cast<int>( dAng ) ).arg( QChar( 176 ) );

            ahrs.resetTransform();
            ahrs.translate( c.dW2, c.dH - c.dW2 - 10.0 );
            if( g_situation.bHaveWTData )
                ahrs.rotate( m_iHeadBugAngle - g_situation.dAHRSMagHeading );
            else
                ahrs.rotate( m_iHeadBugAngle - g_situation.dAHRSGyroHeading );
            ahrs.translate( -c.dW2, -(c.dH - c.dW2 - 10.0) );
            ahrs.setFont( large );
            ahrs.setPen( Qt::black );
            ahrs.drawText( c.dW2 + 5.0, dCrossPos, QString::number( static_cast<int>( dCrossComp ) ) );
            ahrs.setPen( QColor( 0xFF, 0x90, 0x01 ) );
            ahrs.drawText( c.dW2 + 3.0, dCrossPos - 2.0, QString::number( static_cast<int>( dCrossComp ) ) );
            ahrs.setFont( med );
            ahrs.setPen( Qt::black );
            ahrs.drawText( c.dW2 + 5.0, dCrossPos + c.iMedFontHeight - 5, qsCrossAng );
            ahrs.setPen( Qt::cyan );
            ahrs.drawText( c.dW2 + 3.0, dCrossPos + c.iMedFontHeight - 7, qsCrossAng );
        }

        ahrs.resetTransform();
    }

    if( (m_iTimerMin >= 0) && (m_iTimerSec >= 0) )
        paintTimer( &ahrs, &c );

    paintTemp( &ahrs, &c );

    if( m_bShowGPSDetails )
        paintInfo( &ahrs, &c );
    else if( m_bDisplayTanksSwitchNotice )
        paintSwitchNotice( &ahrs, &c );
}


void AHRSCanvas::paintTemp( QPainter *pAhrs, CanvasConstants *c )
{
    // Draw the outside temp if we have it
    if( g_situation.bHaveWTData )
    {
        pAhrs->setPen( Qt::yellow );
        pAhrs->setFont( small );
        pAhrs->drawText( c->dW - c->dW5 - c->dW5, c->dH20 + c->dH80, QString( "%1%2" ).arg( g_situation.dBaroTemp, 0, 'f', 1 ).arg( QChar( 0xB0 ) ) );
    }
}


void AHRSCanvas::paintSwitchNotice( QPainter *pAhrs, CanvasConstants *c )
{
    QLinearGradient cloudyGradient( 0.0, 50.0, 0.0, c->dH - 50.0 );
    QPen            linePen( Qt::black );

    cloudyGradient.setColorAt( 0, QColor( 255, 255, 255, 225 ) );
    cloudyGradient.setColorAt( 1, QColor( 175, 175, 255, 225 ) );

    linePen.setWidth( c->iThickPen );
    pAhrs->setPen( linePen );
    pAhrs->setBrush( cloudyGradient );

    pAhrs->drawRect( c->dW10, c->dH10, (m_bPortrait ? c->dW : c->dWa) - c->dW5, c->dH - c->dH5 );
    pAhrs->setFont( large );
    pAhrs->drawText( c->dW5, c->dH5 + c->iLargeFontHeight, "TANK SWITCH" );
    pAhrs->setFont( med );
    pAhrs->drawText( c->dW5, c->dH5 + (c->iMedFontHeight * 3),  QString( "Switch to  %1  tank" ).arg( m_tanks.bOnLeftTank ? "RIGHT" : "LEFT" ) );
    pAhrs->drawText( c->dW5, c->dH5 + (c->iMedFontHeight * 5),  QString( "ADJUST MIXTURE" ) );
    pAhrs->drawText( c->dW5, c->dH5 + (c->iMedFontHeight * 7),  QString( "PRESS TO CONFIRM" ) );
}


void AHRSCanvas::paintTimer( QPainter *pAhrs, CanvasConstants *c )
{
    QString      qsTimer = QString( "%1:%2" ).arg( m_iTimerMin, 2, 10, QChar( '0' ) ).arg( m_iTimerSec, 2, 10, QChar( '0' ) );
    QFontMetrics largeMetrics( large );
    QPen         linePen( Qt::white );
    QPixmap      Num( c->dW5, c->dH20 );
    QPixmap      timerNum( c->dW5, c->dH20 );

    linePen.setWidth( c->iThinPen );

    pAhrs->setPen( linePen );
    pAhrs->setBrush( Qt::black );

    Builder::buildNumber( &Num, c, qsTimer );
    timerNum.fill( Qt::cyan );
    timerNum.setMask( Num.createMaskFromColor( Qt::transparent ) );

    if( m_bPortrait )
    {
        pAhrs->drawRect( c->dW2 - c->dW10 - c->dW40, c->dH - c->dH20, c->dW5 + c->dW20, c->dH20 );
        pAhrs->drawPixmap( c->dW2 - c->dW10, c->dH - c->dH20 + c->dH100, timerNum );
    }
    else
    {
        pAhrs->drawRect( c->dW2 - c->dW5, c->dH - c->dH5, c->dW5 + c->dW10, c->dH20 );
        pAhrs->drawPixmap( c->dW2 - c->dW10 + c->dW20 - (timerNum.width() / 2), c->dH - c->dH5 + c->dH100, timerNum );
    }
}


void AHRSCanvas::paintInfo( QPainter *pAhrs, CanvasConstants *c )
{
    QLinearGradient cloudyGradient( 0.0, 50.0, 0.0, c->dH - 50.0 );
    QFont           med_bu( med );
    QPen            linePen( Qt::black );
    int             iMedFontHeight = c->iMedFontHeight;
    int             iSmallFontHeight = c->iSmallFontHeight;

    med_bu.setUnderline( true );
    med_bu.setBold( true );

    cloudyGradient.setColorAt( 0, QColor( 255, 255, 255, 225 ) );
    cloudyGradient.setColorAt( 1, QColor( 175, 175, 255, 225 ) );

    linePen.setWidth( c->iThickPen );
    pAhrs->setPen( linePen );
    pAhrs->setBrush( cloudyGradient );
    pAhrs->drawRect( 50, 50, (m_bPortrait ? c->dW : c->dWa) - 100, c->dH - 100 );
    pAhrs->setFont( med_bu );

    pAhrs->drawText( 75, 95, "GPS Status" );
    pAhrs->setFont( small );
    pAhrs->drawText( 75, 95 + iMedFontHeight,  QString( "GPS Satellites Seen: %1" ).arg( g_situation.iGPSSatsSeen ) );
    pAhrs->drawText( 75, 95 + (iMedFontHeight * 2),  QString( "GPS Satellites Tracked: %1" ).arg( g_situation.iGPSSatsTracked ) );
    pAhrs->drawText( 75, 95 + (iMedFontHeight * 3),  QString( "GPS Satellites Locked: %1" ).arg( g_situation.iGPSSats ) );
    pAhrs->drawText( 75, 95 + (iMedFontHeight * 4),  QString( "GPS Fix Quality: %1" ).arg( g_situation.iGPSFixQuality ) );

    QList<StratuxTraffic> trafficList = g_trafficMap.values();
    StratuxTraffic        traffic;
    int                   iY = 0;
    int                   iLine;

    pAhrs->setFont( med_bu );
    pAhrs->drawText( m_bPortrait ? 75 : c->dW, m_bPortrait ? c->dH2 : 95, "Non-ADS-B Traffic" );
    pAhrs->setFont( small );
    foreach( traffic, trafficList )
    {
        // If bearing and distance were able to be calculated then show relative position
        if( !traffic.bHasADSB && (!traffic.qsTail.isEmpty()) )
        {
            iLine = (m_bPortrait ? static_cast<int>( c->dH2 ) : 95) + iMedFontHeight + (iY * iSmallFontHeight);
            pAhrs->drawText( m_bPortrait ? 75 : c->dW, iLine, traffic.qsTail );
            pAhrs->drawText( m_bPortrait ? m_pCanvas->scaledH( 200.0 ) : m_pCanvas->scaledH( 500.0 ), iLine, QString( "%1 ft" ).arg( static_cast<int>( traffic.dAlt ) ) );
            if( traffic.iSquawk > 0 )
                pAhrs->drawText( m_bPortrait ? m_pCanvas->scaledH( 325 ) : m_pCanvas->scaledH( 600 ), iLine, QString::number( traffic.iSquawk ) );
            iY++;
        }
        if( iY > 10 )
            break;
    }

    pAhrs->setFont( med );
    pAhrs->setPen( Qt::blue );
    pAhrs->drawText( 75, m_bPortrait ? m_pCanvas->scaledV( 700.0 ) : m_pCanvas->scaledV( 390.0 ), QString( "Version: %1" ).arg( g_qsStratofierVersion ) );
}

void AHRSCanvas::paintLandscape()
{
    QPainter        ahrs( this );
    CanvasConstants c = m_pCanvas->constants();
    double          dPitchH = c.dH2 + (g_situation.dAHRSpitch / 22.5 * c.dH2);     // The visible portion is only 1/4 of the 90 deg range
    QPolygon        shape;
    QPen            linePen( Qt::black );
    double          dSlipSkid = c.dW2 - ((g_situation.dAHRSSlipSkid / 100.0) * c.dW2);
    double          dPxPerVSpeed = c.dH / 40.0;
    double          dPxPerKnot = static_cast<double>( m_SpeedTape.height() ) / 300.0 * 0.99;
    double          dPxPerFt = static_cast<double>( m_AltTape.height() ) / 20000.0 * 0.99;  // 0.99 accounts for the few pixels above and below the numbers in the pixmap that offset the position at the extremes of the scale
    QFontMetrics    tinyMetrics( tiny );
    QPixmap         num( 320, 84 );

    if( dSlipSkid < (c.dW4 + 25.0) )
        dSlipSkid = c.dW4 + 25.0;
    else if( dSlipSkid > (c.dW2 + c.dW4 - 25.0) )
        dSlipSkid = c.dW2 + c.dW4 - 25.0;

    linePen.setWidth( c.iThinPen );

    ahrs.setRenderHints( QPainter::Antialiasing | QPainter::TextAntialiasing, true );

    // Clip the attitude to the left half of the display
    ahrs.setClipRect( 0, 0, c.dW, c.dH );

    // Translate to dead center and rotate by stratux roll then translate back
    ahrs.translate( c.dW2 - c.dW20, c.dH2 );
    ahrs.rotate( -g_situation.dAHRSroll );
    ahrs.translate( -(c.dW2 - c.dW20), -c.dH2 );

    ahrs.translate( -c.dW20, 0.0 );

    // Top half sky blue gradient offset by stratux pitch
    QLinearGradient skyGradient( 0.0, -c.dH4, 0.0, dPitchH );
    skyGradient.setColorAt( 0, Qt::blue );
    skyGradient.setColorAt( 1, QColor( 85, 170, 255 ) );
    ahrs.fillRect( -400.0, -c.dH4, c.dW + 800.0, dPitchH + c.dH4, skyGradient );

    // Draw brown gradient horizon half offset by stratux pitch
    // Extreme overdraw accounts for extreme roll angles that might expose the corners
    QLinearGradient groundGradient( 0.0, c.dH2, 0, c.dH + c.dH4 );
    groundGradient.setColorAt( 0, QColor( 170, 85, 0 ) );
    groundGradient.setColorAt( 1, Qt::black );

    ahrs.fillRect( -400.0, dPitchH, c.dW + 800.0, c.dH, groundGradient );
    ahrs.setPen( linePen );
    ahrs.drawLine( -400, dPitchH, c.dW + 800.0, dPitchH );

    for( double i = 0.0; i < 20.0; i += 10.0 )
    {
        linePen.setColor( Qt::cyan );
        ahrs.setPen( linePen );
        ahrs.drawLine( c.dW2 - c.dW20, dPitchH - ((i + 2.5) / 45.0 * c.dH2), c.dW2 + c.dW20, dPitchH - ((i + 2.5) / 45.0 * c.dH2) );
        ahrs.drawLine( c.dW2 - c.dW20, dPitchH - ((i + 5.0) / 45.0 * c.dH2), c.dW2 + c.dW20, dPitchH - ((i + 5.0) / 45.0 * c.dH2) );
        ahrs.drawLine( c.dW2 - c.dW20, dPitchH - ((i + 7.5) / 45.0 * c.dH2), c.dW2 + c.dW20, dPitchH - ((i + 7.5) / 45.0 * c.dH2) );
        ahrs.drawLine( c.dW2 - c.dW5, dPitchH - ((i + 10.0) / 45.0 * c.dH2), c.dW2 + c.dW5, dPitchH - (( i + 10.0) / 45.0 * c.dH2) );
        linePen.setColor( QColor( 67, 33, 9 ) );
        ahrs.setPen( linePen );
        ahrs.drawLine( c.dW2 - c.dW20, dPitchH + ((i + 2.5) / 45.0 * c.dH2), c.dW2 + c.dW20, dPitchH + ((i + 2.5) / 45.0 * c.dH2) );
        ahrs.drawLine( c.dW2 - c.dW20, dPitchH + ((i + 5.0) / 45.0 * c.dH2), c.dW2 + c.dW20, dPitchH + ((i + 5.0) / 45.0 * c.dH2) );
        ahrs.drawLine( c.dW2 - c.dW20, dPitchH + ((i + 7.5) / 45.0 * c.dH2), c.dW2 + c.dW20, dPitchH + ((i + 7.5) / 45.0 * c.dH2) );
        ahrs.drawLine( c.dW2 - c.dW5, dPitchH + ((i + 10.0) / 45.0 * c.dH2), c.dW2 + c.dW5, dPitchH + (( i + 10.0) / 45.0 * c.dH2) );
    }

    ahrs.translate( c.dW20, 0.0 );

    // Reset rotation
    ahrs.resetTransform();

    // Remove the clipping rect
    ahrs.setClipping( false );

    ahrs.translate( -c.dW20, 0.0 );

    // Slip/Skid indicator
    drawSlipSkid( &ahrs, &c, dSlipSkid );

    // Draw the top roll indicator
    ahrs.translate( c.dW2, c.dH20 + ((c.dW - c.dW5) / 2.0) );
    ahrs.rotate( -g_situation.dAHRSroll );
    ahrs.translate( -c.dW2, -(c.dH20 + ((c.dW - c.dW5) / 2.0)) );
    ahrs.drawPixmap( c.dW2 - ((c.dW - c.dW5) / 2.0), c.dH20, c.dW - c.dW5, c.dW - c.dW5, m_RollIndicator );
    ahrs.resetTransform();

    ahrs.translate( -c.dW20, 0.0 );

    QPolygonF arrow;

    arrow.append( QPointF( c.dW2, c.dH10 ) );
    arrow.append( QPointF( c.dW2 + c.dW40, c.dH10 + c.dH40 ) );
    arrow.append( QPointF( c.dW2 - c.dW40, c.dH10 + c.dH40 ) );
    ahrs.setBrush( Qt::white );
    ahrs.setPen( Qt::black );
    ahrs.drawPolygon( arrow );

    // Draw the yellow pitch indicators
    ahrs.setBrush( Qt::yellow );
    shape.append( QPoint( c.dW5 + c.dW10, c.dH2 - c.dH160 - 4.0 ) );
    shape.append( QPoint( c.dW2 - c.dW10, c.dH2 - c.dH160 - 4.0 ) );
    shape.append( QPoint( c.dW2 - c.dW10 + 20, c.dH2 ) );
    shape.append( QPoint( c.dW2 - c.dW10, c.dH2 + c.dH160 + 4.0 ) );
    shape.append( QPoint( c.dW5 + c.dW10, c.dH2 + c.dH160 + 4.0 ) );
    ahrs.drawPolygon( shape );
    shape.clear();
    shape.append( QPoint( c.dW - c.dW5 - c.dW10, c.dH2 - c.dH160 - 4.0 ) );
    shape.append( QPoint( c.dW2 + c.dW10, c.dH2 - c.dH160 - 4.0 ) );
    shape.append( QPoint( c.dW2 + c.dW10 - 20, c.dH2 ) );
    shape.append( QPoint( c.dW2 + c.dW10, c.dH2 + c.dH160 + 4.0 ) );
    shape.append( QPoint( c.dW - c.dW5 - c.dW10, c.dH2 + c.dH160 + 4.0 ) );
    ahrs.drawPolygon( shape );
    shape.clear();
    shape.append( QPoint( c.dW2, c.dH2 ) );
    shape.append( QPoint( c.dW2 - c.dW20, c.dH2 + 20 ) );
    shape.append( QPoint( c.dW2 + c.dW20, c.dH2 + 20 ) );
    ahrs.drawPolygon( shape );

    ahrs.translate( c.dW20, 0.0 );

    // Arrow for heading position above heading dial
    arrow.clear();
    arrow.append( QPointF( c.dW + c.dW2, c.dH - c.dW - c.dH80 ) );
    arrow.append( QPointF( c.dW + c.dW2 + c.dW40, c.dH - c.dW - c.dH40 ) );
    arrow.append( QPointF( c.dW + c.dW2 - c.dW40, c.dH - c.dW - c.dH40 ) );
    ahrs.setBrush( Qt::white );
    ahrs.setPen( Qt::black );
    ahrs.drawPolygon( arrow );

    // Draw the heading pixmap and rotate it to the current heading
    ahrs.translate( c.dW + c.dW2, c.dH - c.dW2 - 10.0 );
    if( g_situation.bHaveWTData )
        ahrs.rotate( -g_situation.dAHRSMagHeading );
    else
        ahrs.rotate( -g_situation.dAHRSGyroHeading );
    ahrs.translate( -(c.dW + c.dW2), -(c.dH - c.dW2 - 10.0) );
    ahrs.drawPixmap( c.dW + 10, c.dH - c.dW, c.dW - 20, c.dW - 20,  m_HeadIndicator );
    ahrs.resetTransform();

    drawDirectOrFromTo( &ahrs, &c );

    // Draw the central airplane
    ahrs.drawPixmap( c.dW + c.dW2 - c.dW20, c.dH - c.dW2 - c.dW20 - 20.0, c.dW10, c.dW10, m_planeIcon );

    // Draw the heading bug
    if( m_iHeadBugAngle >= 0 )
    {
        ahrs.translate( c.dW + c.dW2, c.dH - c.dW2 - 20.0 );
        if( g_situation.bHaveWTData )
            ahrs.rotate( m_iHeadBugAngle - g_situation.dAHRSMagHeading );
        else
            ahrs.rotate( m_iHeadBugAngle - g_situation.dAHRSGyroHeading );
        ahrs.translate( -(c.dW + c.dW2), -(c.dH - c.dW2 - 20.0) );
        ahrs.drawPixmap( c.dW + c.dW2 - (m_headIcon.width() / 2), c.dH - c.dH10 - c.dW, m_headIcon );

        // If long press triggered crosswind component display and the wind bug is set
        if( m_bShowCrosswind && (m_iWindBugAngle >= 0) )
        {
            linePen.setWidth( c.iThinPen );
            linePen.setColor( QColor( 0xFF, 0x90, 0x01 ) );
            ahrs.setPen( linePen );
            ahrs.drawLine( c.dW + c.dW2, c.dH - c.dW - 10.0, c.dW + c.dW2, c.dH - 10.0 - c.dW2 );
        }

        ahrs.resetTransform();
    }

    // Draw the wind bug
    if( m_iWindBugAngle >= 0 )
    {
        ahrs.translate( c.dW + c.dW2, c.dH - c.dW2 - 20.0 );
        if( g_situation.bHaveWTData )
            ahrs.rotate( m_iWindBugAngle - g_situation.dAHRSMagHeading );
        else
            ahrs.rotate( m_iWindBugAngle - g_situation.dAHRSGyroHeading );
        ahrs.translate( -(c.dW + c.dW2), -(c.dH - c.dW2 - 20.0) );
        ahrs.drawPixmap( c.dW + c.dW2 - (m_headIcon.width() / 2), c.dH - c.dH10 - c.dW, m_windIcon );

        QString      qsWind = QString::number( m_iWindBugSpeed );
        QFontMetrics windMetrics( med );
        QRect        windRect = windMetrics.boundingRect( qsWind );

        ahrs.setFont( med );
        ahrs.setPen( Qt::black );
        ahrs.drawText( c.dW + c.dW2 - (windRect.width() / 2), c.dH - c.dW - 3, qsWind );
        ahrs.setPen( Qt::white );
        ahrs.drawText( c.dW + c.dW2 - (windRect.width() / 2) - 1, c.dH - c.dW - 4, qsWind );

        // If long press triggered crosswind component display and the heading bug is set
        if( m_bShowCrosswind && (m_iHeadBugAngle >= 0) )
        {
            linePen.setWidth( c.iThinPen );
            linePen.setColor( Qt::cyan );
            ahrs.setPen( linePen );
            ahrs.drawLine( c.dW + c.dW2, c.dH - c.dW - 10, c.dW + c.dW2, c.dH - 10.0 - c.dW2 );

            // Draw the crosswind component calculated from heading vs wind
            double dAng = fabs( static_cast<double>( m_iWindBugAngle ) - static_cast<double>( m_iHeadBugAngle ) );
            while( dAng > 180.0 )
                dAng -= 360.0;
            dAng = fabs( dAng );
            double dCrossComp = fabs( static_cast<double>( m_iWindBugSpeed ) * sin( dAng * ToRad ) );
            double dCrossPos = c.dH - (c.dW / 1.3) - 10.0;
            QString qsCrossAng = QString( "%1%2" ).arg( static_cast<int>( dAng ) ).arg( QChar( 176 ) );

            ahrs.resetTransform();
            ahrs.translate( c.dW + c.dW2, c.dH - c.dW2 - 10.0 );
            if( g_situation.bHaveWTData )
                ahrs.rotate( m_iHeadBugAngle - g_situation.dAHRSMagHeading );
            else
                ahrs.rotate( m_iHeadBugAngle - g_situation.dAHRSGyroHeading );
            ahrs.translate( -(c.dW + c.dW2), -(c.dH - c.dW2 - 10.0) );
            ahrs.setFont( large );
            ahrs.setPen( Qt::black );
            ahrs.drawText( c.dW + c.dW2 + 5.0, dCrossPos, QString::number( static_cast<int>( dCrossComp ) ) );
            ahrs.setPen( QColor( 0xFF, 0x90, 0x01 ) );
            ahrs.drawText( c.dW + c.dW2 + 3.0, dCrossPos - 2.0, QString::number( static_cast<int>( dCrossComp ) ) );
            ahrs.setFont( med );
            ahrs.setPen( Qt::black );
            ahrs.drawText( c.dW + c.dW2 + 5.0, dCrossPos + c.iMedFontHeight - 5, qsCrossAng );
            ahrs.setPen( Qt::cyan );
            ahrs.drawText( c.dW + c.dW2 + 3.0, dCrossPos + c.iMedFontHeight - 7, qsCrossAng );
        }

        ahrs.resetTransform();
    }

    // Draw the Altitude tape
    ahrs.fillRect( c.dW - c.dW5 - c.dW40, 0, c.dW5 + c.dW40, c.dH, QColor( 0, 0, 0, 100 ) );
    ahrs.drawPixmap( c.dW - c.dW5 - c.dW40 + 5, c.dH2 + 10.0 - m_AltTape.height() + (g_situation.dBaroPressAlt * dPxPerFt) , m_AltTape );

    // Draw the altitude bug
    if( m_iAltBug >= 0 )
    {
        double dAltTip = c.dH2 - 10.0 - ((static_cast<double>( m_iAltBug ) - g_situation.dBaroPressAlt) * dPxPerFt);

        ahrs.drawPixmap( c.dW - c.dW5 - c.dW20 - c.dW40, dAltTip - c.dH40, c.dW20, c.dH20, m_AltBug );
    }

    // Draw the vertical speed static pixmap
    ahrs.fillRect( c.dW - c.dW20 - c.dW40, 0.0, c.dW40, c.dH, QColor( 0, 0, 0, 100 ) );
    ahrs.drawPixmap( c.dW - c.dW20, 0, c.dW20, c.dH, m_VertSpeedTape );

    // Draw the vertical speed indicator
    ahrs.translate( 0.0, c.dH2 - (dPxPerVSpeed * g_situation.dGPSVertSpeed / 100.0 * 0.98) );   // 98% accounts for the slight margin on each end
    arrow.clear();
    arrow.append( QPoint( c.dW - m_pCanvas->scaledH( 30.0 ), 0.0 ) );
    arrow.append( QPoint( c.dW - m_pCanvas->scaledH( 20.0 ), m_pCanvas->scaledV( -7.0 ) ) );
    arrow.append( QPoint( c.dW, m_pCanvas->scaledV( -15.0 ) ) );
    arrow.append( QPoint( c.dW, m_pCanvas->scaledV( 15.0 ) ) );
    arrow.append( QPoint( c.dW - m_pCanvas->scaledH( 20.0 ), m_pCanvas->scaledV( 7.0 ) ) );
    ahrs.setPen( Qt::black );
    ahrs.setBrush( Qt::white );
    ahrs.drawPolygon( arrow );

    QString qsFullVspeed = QString::number( g_situation.dGPSVertSpeed / 100.0, 'f', 1 );
    QString qsFracVspeed = qsFullVspeed.right( 1 );
    QString qsIntVspeed = qsFullVspeed.left( qsFullVspeed.length() - 2 );
    QFontMetrics weeMetrics( wee );

    // Draw vertical speed indicator as In thousands and hundreds of FPM in tiny text on the vertical speed arrow
    ahrs.setFont( wee );
    QRect intRect( weeMetrics.boundingRect( qsIntVspeed ) );

    ahrs.drawText( c.dW - m_pCanvas->scaledH( 20.0 ), m_pCanvas->scaledV( 4.0 ), qsIntVspeed );
    ahrs.setFont( itsy );
    ahrs.drawText( c.dW - m_pCanvas->scaledH( 20.0 ) + intRect.width() + 2, m_pCanvas->scaledV( 4.0 ), qsFracVspeed );
    ahrs.resetTransform();

    // Draw the current altitude
    Builder::buildNumber( &num, &c, static_cast<int>( g_situation.dBaroPressAlt ), 0 );
    drawCurrAlt( &ahrs, &c, &num );

    // Draw the Speed tape
    ahrs.fillRect( 0, 0, c.dW10 + 5.0, c.dH, QColor( 0, 0, 0, 100 ) );
    ahrs.drawPixmap( 5, c.dH2 + 5.0 - m_SpeedTape.height() + (g_situation.dGPSGroundSpeed * dPxPerKnot), m_SpeedTape );

    // Draw the current speed
    if( g_situation.bHaveWTData )
    {
        Builder::buildNumber( &num, &c, static_cast<int>( g_situation.dTAS ), 0 );
        drawCurrSpeed( &ahrs, &c, &num );
        Builder::buildNumber( &num, &c, static_cast<int>( g_situation.dGPSGroundSpeed ), 0 );

        // Draw the ground speed just below the indicator since we have both, and both are useful
        drawCurrSpeed( &ahrs, &c, &num, true );
    }
    else
    {
        Builder::buildNumber( &num, &c, static_cast<int>( g_situation.dGPSGroundSpeed ), 0 );
        drawCurrSpeed( &ahrs, &c, &num );
    }

    ahrs.setFont( wee );
    ahrs.setPen( Qt::black );
    QString qsUnits( speedUnits() );
    ahrs.drawText( c.dW10 + c.dW40 + (c.dW80 / 2.0) + 1, c.dH2 + c.dH80 + 1, qsUnits );
    ahrs.setPen( Qt::white );
    ahrs.drawText( c.dW10 + c.dW40 + (c.dW80 / 2.0), c.dH2 + c.dH80, qsUnits );

    // Draw the heading value over the indicator
    ahrs.setPen( QPen( Qt::white, c.iThinPen ) );
    ahrs.setBrush( Qt::black );
    ahrs.drawRect( c.dW + c.dW2 - (c.dWNum * 3.0 / 2.0) - (c.dW * 0.0125), 10.0, (c.dWNum * 3.0) + (c.dW * 0.025), c.dHNum + (c.dH * 0.015) );
    if( g_situation.bHaveWTData )
        Builder::buildNumber( &num, &c, static_cast<int>( g_situation.dAHRSMagHeading ), 3 );
    else
        Builder::buildNumber( &num, &c, static_cast<int>( g_situation.dAHRSGyroHeading ), 3 );
    ahrs.drawPixmap( c.dW + c.dW2 - (c.dWNum * 3.0 / 2.0), 10.0 + (c.dH * 0.0075), num );

    // Draw the G-Force indicator box and scale
    ahrs.setFont( tiny );
    ahrs.setPen( Qt::black );
    ahrs.drawText( c.dW2 - c.dW20 - c.dW10 - (c.iTinyFontWidth / 2) + 1, c.dH - 15, "0" );
    ahrs.drawText( c.dW2 - c.dW20 - (c.iTinyFontWidth / 2) + 1, c.dH - 15, "1" );
    ahrs.drawText( c.dW2 - c.dW20 + c.dW10 - (c.iTinyFontWidth / 2) + 1, c.dH - 15, "2" );
    ahrs.setPen( Qt::white );
    ahrs.drawText( c.dW2 - c.dW20 - c.dW10 - (c.iTinyFontWidth / 2), c.dH - 16, "0" );
    ahrs.drawText( c.dW2 - c.dW20 - (c.iTinyFontWidth / 2), c.dH - 16, "1" );
    ahrs.drawText( c.dW2 - c.dW20 + c.dW10 - (c.iTinyFontWidth / 2), c.dH - 16, "2" );

    // Arrow for G-Force indicator
    arrow.clear();
    arrow.append( QPoint( 0, c.dH - c.iTinyFontHeight - 10.0 ) );
    arrow.append( QPoint( -14.0, c.dH - c.iTinyFontHeight - 25.0 ) );
    arrow.append( QPoint( 16.0, c.dH - c.iTinyFontHeight - 25.0 ) );
    ahrs.setPen( Qt::black );
    ahrs.setBrush( Qt::white );
    ahrs.translate( (fabs( 1.0 - g_situation.dAHRSGLoad ) * (c.dW5 + c.dW10) * 20.0) + c.dW2 - c.dW20 - c.dW10, -c.dH80 );
    ahrs.drawPolygon( arrow );
    ahrs.resetTransform();

    // Left Tank indicators background
    ahrs.drawPixmap( c.dW10 + c.dW20, c.dH2 + c.dH10, c.dW20, c.dH2 - c.dH5, m_Lfuel );
    // Tank indicators level
    QPen levelPen( Qt::black, c.dH40 + 4 );
    levelPen.setCapStyle( Qt::RoundCap );
    ahrs.setPen( levelPen );
    ahrs.drawLine( c.dW10 + c.dW20,
                   c.dH2 + c.dH10 + ((c.dH2 - c.dH5) * ((m_tanks.dLeftCapacity - m_tanks.dLeftRemaining) / m_tanks.dLeftCapacity)),
                   c.dW40 + c.dW10 + c.dW20,
                   c.dH2 + c.dH10 + ((c.dH2 - c.dH5) * ((m_tanks.dLeftCapacity - m_tanks.dLeftRemaining) / m_tanks.dLeftCapacity)) );
    levelPen.setWidth( c.dH40 );
    levelPen.setColor( QColor( 255, 150, 255 ) );
    ahrs.setPen( levelPen );
    ahrs.drawLine( c.dW10 + c.dW20,
                   c.dH2 + c.dH10 + ((c.dH2 - c.dH5) * ((m_tanks.dLeftCapacity - m_tanks.dLeftRemaining) / m_tanks.dLeftCapacity)),
                   c.dW40 + c.dW10 + c.dW20,
                   c.dH2 + c.dH10 + ((c.dH2 - c.dH5) * ((m_tanks.dLeftCapacity - m_tanks.dLeftRemaining) / m_tanks.dLeftCapacity)) );
    // Right Tank indicators background
    ahrs.drawPixmap( c.dW - c.dW5 - c.dW10 - 1, c.dH2 + c.dH10, c.dW20, c.dH2 - c.dH5, m_Rfuel );
    // Right Tank indicators level
    levelPen.setColor( Qt::black );
    levelPen.setWidth( c.dH40 + 4 );
    ahrs.setPen( levelPen );
    ahrs.drawLine( c.dW - c.dW5 - c.dW20,
                   c.dH2 + c.dH10 + ((c.dH2 - c.dH5) * ((m_tanks.dRightCapacity - m_tanks.dRightRemaining) / m_tanks.dRightCapacity)),
                   c.dW - c.dW40 - c.dW5 - c.dW20 - 1,
                   c.dH2 + c.dH10 + ((c.dH2 - c.dH5) * ((m_tanks.dRightCapacity - m_tanks.dRightRemaining) / m_tanks.dRightCapacity)) );
    levelPen.setWidth( c.dH40 );
    levelPen.setColor( QColor( 255, 150, 255 ) );
    ahrs.setPen( levelPen );
    ahrs.drawLine( c.dW - c.dW5 - c.dW20,
                   c.dH2 + c.dH10 + ((c.dH2 - c.dH5) * ((m_tanks.dRightCapacity - m_tanks.dRightRemaining) / m_tanks.dRightCapacity)),
                   c.dW - c.dW40 - c.dW5 - c.dW20 - 1,
                   c.dH2 + c.dH10 + ((c.dH2 - c.dH5) * ((m_tanks.dRightCapacity - m_tanks.dRightRemaining) / m_tanks.dRightCapacity)) );

    // Tank indicator active indicators
    ahrs.setFont( large );
    if( m_bFuelFlowStarted )
    {
        QPen fuelPen( Qt::yellow, c.dH80 );

        ahrs.setPen( fuelPen );

        if( !m_tanks.bDualTanks )
        {
            ahrs.drawLine( c.dW10, c.dH2 + c.dH10 - c.dH80 - 5, c.dW10, c.dH2 + c.dH10 - c.dH80 - 5 );
            ahrs.drawLine( c.dW - c.dW5 - c.dW10, c.dH2 + c.dH10 - c.dH80 - 5, c.dW - c.dW5, c.dH2 + c.dH10 - c.dH80 - 5 );
        }
        else
        {
            if( m_tanks.bOnLeftTank )
                ahrs.drawLine( c.dW10, c.dH2 + c.dH10 - 15, c.dW5, c.dH2 + c.dH10 - 15 );
            else
                ahrs.drawLine( c.dW - c.dW5 - c.dW10, c.dH2 + c.dH10 - 15, c.dW - c.dW5, c.dH2 + c.dH10 - 15 );
        }
    }

    // Update the airspace positions
    updateAirspaces( &ahrs, &c );

    // Update the airport positions
    if( m_settings.eShowAirports != Canvas::ShowNoAirports )
        updateAirports( &ahrs, &c );

    // Update the traffic positions
    updateTraffic( &ahrs, &c );

    ahrs.drawPixmap( c.dW + c.dW40, c.dH - c.dH20 - c.dH40, c.dH20, c.dH20, m_DirectTo );
    ahrs.drawPixmap( c.dW + c.dW40 + c.dH20, c.dH - c.dH20 - c.dH40, c.dH20, c.dH20, m_FromTo );

    // Draw the heading overlay so the markers aren't covered by other elements
    ahrs.translate( c.dW + c.dW2, c.dH - c.dW2 - 10.0 );
    if( g_situation.bHaveWTData )
        ahrs.rotate( -g_situation.dAHRSMagHeading );
    else
        ahrs.rotate( -g_situation.dAHRSGyroHeading );
    ahrs.translate( -(c.dW + c.dW2), -(c.dH - c.dW2 - 10.0) );
    ahrs.drawPixmap( c.dW + 10, c.dH - c.dW, c.dW - 20, c.dW - 20,  m_HeadIndicatorOverlay );
    ahrs.resetTransform();

    if( (m_iTimerMin >= 0) && (m_iTimerSec >= 0) )
        paintTimer( &ahrs, &c );

    paintTemp( &ahrs, &c );

    if( m_bShowGPSDetails )
        paintInfo( &ahrs, &c );
    else if( m_bDisplayTanksSwitchNotice )
        paintSwitchNotice( &ahrs, &c );
}


void AHRSCanvas::drawSlipSkid( QPainter *pAhrs, CanvasConstants *pC, double dSlipSkid )
{
    pAhrs->setPen( QPen( Qt::white, 2 ) );
    pAhrs->setBrush( Qt::black );
    pAhrs->drawRect( pC->dW2 - pC->dW4, 1, pC->dW2, pC->dH40 );
    pAhrs->drawRect( pC->dW2 - 15.0, 1.0, 30.0, pC->dH40 );
    pAhrs->setPen( Qt::NoPen );
    pAhrs->setBrush( Qt::green );
    pAhrs->drawEllipse( dSlipSkid - 7.0,
                        1.0,
                        20.0,
                        pC->dH40 );
}


void AHRSCanvas::drawCurrAlt( QPainter *pAhrs, CanvasConstants *pC, QPixmap *pNum )
{
    if( pC->bPortrait )
        pAhrs->translate( 0.0, -pC->dH4 );

    pAhrs->setPen( QPen( Qt::white, pC->iThinPen ) );
    pAhrs->setBrush( QColor( 0, 0, 0, 175 ) );
    pAhrs->drawRect( pC->dW - pC->dW5 - pC->dW40, pC->dH2 - (pC->dHNum / 2.0) - (pC->dH * 0.0075), pC->dW5 + pC->dW40, pC->dHNum + (pC->dH * 0.015) );
    pAhrs->setPen( Qt::white );
    pAhrs->drawPixmap( pC->dW - pC->dW5, pC->dH2 - (pC->dHNum / 2.0), *pNum );

    if( pC->bPortrait )
        pAhrs->resetTransform();
}


void AHRSCanvas::drawCurrSpeed( QPainter *pAhrs, CanvasConstants *pC, QPixmap *pNum, bool bGS )
{
    if( pC->bPortrait )
        pAhrs->translate( 0.0, -pC->dH4 );

    if( !bGS )
    {
        pAhrs->setPen( QPen( Qt::white, pC->iThinPen ) );
        pAhrs->setBrush( QColor( 0, 0, 0, 175 ) );
        pAhrs->drawRect( 0, pC->dH2 - (pC->dHNum / 2.0) - (pC->dH * 0.0075), pC->dW5 + pC->dW80, pC->dHNum + (pC->dH * 0.015) );
        pAhrs->drawPixmap( pC->dW * 0.0125, pC->dH2 - (pC->dHNum / 2.0), *pNum );
    }
    else
        pAhrs->drawPixmap( pC->dW10, pC->dH2 + pC->dH40, *pNum );

    if( pC->bPortrait )
        pAhrs->resetTransform();
}

void AHRSCanvas::timerReminder( int iMinutes, int iSeconds )
{
    CanvasConstants c = m_pCanvas->constants();

    m_iTimerMin = iMinutes;
    m_iTimerSec = iSeconds;

    if( (iMinutes == 0) && (iSeconds == 0) )
    {
        TimerDialog  dlg( this );

        dlg.setGeometry( c.dW2 - c.dW5, c.dH - c.dW2 - 20 - c.dH5, c.dW5 + c.dW5, c.dH5 + c.dH5 );

        int          iSel = dlg.exec();
        AHRSMainWin *pMainWin = static_cast<AHRSMainWin *>( parentWidget()->parentWidget() );

        if( iSel == QDialog::Rejected )
        {
            m_iTimerMin = m_iTimerSec = -1;
            return;
        }
        else if( iSel == TimerDialog::Restart )
            pMainWin->restartTimer();
        else if( iSel == TimerDialog::Change )
            pMainWin->changeTimer();
    }
}


void AHRSCanvas::updateAirports( QPainter *pAhrs, CanvasConstants *c )
{
    Airport      ap;
    QLineF       ball;
    double	     dPxPerNM = static_cast<double>( c->dW - 30.0 ) / (m_dZoomNM * 2.0);	// Pixels per nautical mile; the outer limit of the heading indicator is calibrated to the zoom level in NM
    double       dAPDist;
    QPen         apPen( Qt::magenta );
    QLineF       runwayLine;
    int          iRunway, iAPRunway;
    double       dAirportDiam = c->dWa * (m_bPortrait ? 0.03125 : 0.01875);
    QPainterPath maskPath;
    QRect        apRect;
    double       deltaY;
    double       dHead = g_situation.dAHRSGyroHeading;

    if( g_situation.bHaveWTData )
        dHead = g_situation.dAHRSMagHeading;

    QFontMetrics apMetrics( tiny );

    pAhrs->setFont( tiny );

    maskPath.addEllipse( 20.0 + (m_bPortrait ? 0 : c->dW),
                         c->dH - c->dW,
                         c->dW - 40.0, c->dW - 40.0 );
    pAhrs->setClipPath( maskPath );

    apPen.setWidth( c->iThinPen );
    pAhrs->setBrush( Qt::NoBrush );

    foreach( ap, m_airports )
    {
        if( ap.bGrass && (m_settings.eShowAirports == Canvas::ShowPavedAirports) )
            continue;
        else if( (!ap.bGrass) && (m_settings.eShowAirports == Canvas::ShowGrassAirports) )
            continue;

        apRect = apMetrics.boundingRect( ap.qsID );

        dAPDist = ap.bd.dDistance * dPxPerNM;

        ball.setP1( QPointF( (m_bPortrait ? 0.0 : c->dW) + c->dW2, c->dH - c->dW2 - 30.0 ) );
        ball.setP2( QPointF( (m_bPortrait ? 0.0 : c->dW) + c->dW2, c->dH - c->dW2 - 30.0 + dAPDist ) );

        // Airport angle in reference to you (which clock position it's at)
        ball.setAngle( ap.bd.dBearing - dHead - 90.0 );
        // Qt Y coords are backward
        deltaY = ball.p2().y() - (c->dH - c->dW2 - 30.0);
        ball.setP2( QPointF( ball.p2().x(), c->dH - c->dW2 - 30.0 - deltaY ) );

        apPen.setWidth( c->iThinPen );
        apPen.setColor( Qt::black );
        pAhrs->setPen( apPen );
        pAhrs->drawEllipse( ball.p2().x() - (dAirportDiam / 2.0) + 1, ball.p2().y() - (dAirportDiam / 2.0) + 1, dAirportDiam, dAirportDiam );
        apPen.setColor( Qt::magenta );
        pAhrs->setPen( apPen );
        pAhrs->drawEllipse( ball.p2().x() - (dAirportDiam / 2.0), ball.p2().y() - (dAirportDiam / 2.0), dAirportDiam, dAirportDiam );
        apPen.setColor( Qt::black );
        pAhrs->setPen( apPen );
        pAhrs->drawText( ball.p2().x() - (dAirportDiam / 2.0) - (apRect.width() / 2) + 2, ball.p2().y() - (dAirportDiam / 2.0) + apRect.height() - 1, ap.qsID );
        // Draw the runways and tiny headings after the black ID shadow but before the yellow ID text
        if( (m_dZoomNM <= 30) && m_settings.bShowRunways )
        {
            for( iRunway = 0; iRunway < ap.runways.count(); iRunway++ )
            {
                iAPRunway = ap.runways.at( iRunway );
                runwayLine.setP1( QPointF( ball.p2().x(), ball.p2().y() ) );
                runwayLine.setP2( QPointF( ball.p2().x(), ball.p2().y() + (dAirportDiam * 2.0) ) );
                runwayLine.setAngle( 270.0 - static_cast<double>( iAPRunway ) );
                apPen.setColor( Qt::magenta );
                apPen.setWidth( c->iThickPen );
                pAhrs->setPen( apPen );
                pAhrs->drawLine( runwayLine );
                if( ((iAPRunway - dHead) > 90) && ((iAPRunway - dHead) < 270) )
                    runwayLine.setLength( runwayLine.length() + c->dW80 );
                QPixmap num( 128, 84 ); // It would need to be resized back anyway so creating a new one each time is faster
                num.fill( Qt::transparent );
                Builder::buildNumber( &num, c, iAPRunway / 10, 2 );
                num = num.scaledToWidth( c->dW20, Qt::SmoothTransformation );
                pAhrs->drawPixmap( runwayLine.p2(), num );
            }
        }
        apPen.setColor( Qt::yellow );
        pAhrs->setPen( apPen );
        pAhrs->drawText( ball.p2().x() - (dAirportDiam / 2.0) - (apRect.width() / 2) + 1, ball.p2().y() - (dAirportDiam / 2.0) + apRect.height() - 2, ap.qsID );
    }

    pAhrs->setClipping( false );
}


void AHRSCanvas::updateAirspaces( QPainter *pAhrs, CanvasConstants *c )
{
    if( !m_settings.bShowAirspaces )
        return;

    Airspace     as;
    QLineF       ball;
    double	     dPxPerNM = static_cast<double>( c->dW - 30.0 ) / (m_dZoomNM * 2.0);	// Pixels per nautical mile; the outer limit of the heading indicator is calibrated to the zoom level in NM
    double       dASDist;
    QPen         asPen( Qt::yellow );
    QPainterPath maskPath;
    BearingDist  bd;
    QPolygonF    airspacePoly;
    double       deltaY;

    maskPath.addEllipse( 20.0 + (m_bPortrait ? 0 : c->dW),
                         c->dH - c->dW,
                         c->dW - 40.0, c->dW - 40.0 );
    pAhrs->setClipPath( maskPath );

    asPen.setWidth( c->iThinPen );

    foreach( as, m_airspaces )
    {
        airspacePoly.clear();
        foreach( bd, as.shapeHav )
        {
            dASDist = bd.dDistance * dPxPerNM;

            ball.setP1( QPointF( (m_bPortrait ? 0.0 : c->dW) + c->dW2, c->dH - c->dW2 - 30.0 ) );
            ball.setP2( QPointF( (m_bPortrait ? 0.0 : c->dW) + c->dW2, c->dH - c->dW2 - 30.0 - dASDist ) );

            // Airspace polygon point angle in reference to you (which clock position it's at)
            if( g_situation.bHaveWTData )
                ball.setAngle( bd.dBearing - g_situation.dAHRSMagHeading - 90.0 );
            else
                ball.setAngle( bd.dBearing - g_situation.dAHRSGyroHeading - 90.0 );
            // Qt Y coords are backward
            deltaY = ball.p2().y() - (c->dH - c->dW2 - 30.0);
            ball.setP2( QPointF( ball.p2().x(), c->dH - c->dW2 - 30.0 - deltaY ) );
            airspacePoly.append( ball.p2() );
        }
        pAhrs->setBrush( Qt::NoBrush );
        switch( as.eType )
        {
            case Canvas::Airspace_Class_B:
                asPen.setColor( Qt::blue );
                break;
            case Canvas::Airspace_Class_C:
                asPen.setColor( Qt::darkBlue );
                break;
            case Canvas::Airspace_Class_D:
                asPen.setColor( Qt::green );
                break;
            case Canvas::Airspace_Class_E:
                asPen.setColor( Qt::darkGreen );
                break;
            case Canvas::Airspace_Class_G:
                asPen.setColor( Qt::gray );
                break;
            case Canvas::Airspace_MOA:
                asPen.setColor( Qt::darkMagenta );
                break;
            case Canvas::Airspace_TFR:
                asPen.setColor( Qt::darkYellow );
                pAhrs->setBrush( QColor( 255, 255, 0, 50 ) );
                break;
            case Canvas::Airspace_SFRA:
                asPen.setColor( Qt::red );
                pAhrs->setBrush( QColor( 255, 0, 0, 50 ) );
                break;
            case Canvas::Airspace_Prohibited:
                asPen.setColor( Qt::darkCyan );
                pAhrs->setBrush( QColor( 0, 255, 255, 50 ) );
                break;
            case Canvas::Airspace_Restricted:
                asPen.setColor( QColor( 0xFF, 0xA5, 0x00 ) );
                pAhrs->setBrush( QColor( 0xFF, 0xA5, 0x00, 50 ) );
                break;
            default:
                asPen.setColor( Qt::transparent );
                break;
        }
        pAhrs->setPen( asPen );
        pAhrs->drawPolygon( airspacePoly );
        if( (as.iAltTop > 0) && m_settings.bShowAltitudes )
        {
            QRectF asBound = airspacePoly.boundingRect();
            QLineF asLine( asBound.topRight(), asBound.bottomLeft() );
            while( !airspacePoly.containsPoint( asLine.p2(), Qt::OddEvenFill ) )
                asLine.setLength( asLine.length() - 1.0 );
            asLine.setLength( asLine.length() - 2.0 );
            pAhrs->setPen( Qt::darkGray );
            pAhrs->setFont( itsy );
            pAhrs->drawText( QPointF( asLine.p2().x(), asLine.p2().y() - c->iTinyFontHeight ), QString::number( as.iAltTop / 100 ) );
            if( as.iAltBottom <= 0 )
                pAhrs->drawText( asLine.p2(), "GND" );
            else
                pAhrs->drawText( asLine.p2(), QString::number( as.iAltBottom / 100 ) );
        }
    }
    pAhrs->setClipping( false );
}


void AHRSCanvas::orient( bool bPortrait )
{
    m_bPortrait = bPortrait;
    QTimer::singleShot( 1000, this, SLOT( orient2() ) );
}


void AHRSCanvas::orient2()
{
    if( !m_bInitialized )
        return;

    m_bInitialized = false;

    m_pCanvas->init( width(), height(), m_bPortrait );

    CanvasConstants c = m_pCanvas->constants();
    int             iBugSize = static_cast<int>( c.dWa * (m_bPortrait ? 0.1333 : 0.08) );

    // Reload the icons so we don't loose resolution
    m_headIcon.load( ":/icons/resources/HeadingIcon.png" );
    m_windIcon.load( ":/icons/resources/WindIcon.png" );
    // Rescale them
    m_headIcon = m_headIcon.scaled( iBugSize, iBugSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation );
    m_windIcon = m_windIcon.scaled( iBugSize, iBugSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation );

    if( m_bPortrait )
        m_VertSpeedTape.load( ":/graphics/resources/vspeedP.png" );
    else
        m_VertSpeedTape.load( ":/graphics/resources/vspeedL.png" );

    // Reload these for the same reason as above
    m_AltTape.load( ":/graphics/resources/alttape.png" );
    m_SpeedTape.load( ":/graphics/resources/speedtape.png" );
    // Rescale them
    m_AltTape = m_AltTape.scaledToWidth( c.dW10, Qt::SmoothTransformation );
    m_SpeedTape = m_SpeedTape.scaledToWidth( c.dW10 - c.dW40, Qt::SmoothTransformation );
    QTransform flipper;
    flipper.scale( -1, 1 );
    m_Rfuel = m_Lfuel.transformed( flipper );

    m_bInitialized = true;
}


void AHRSCanvas::swipeLeft()
{
    AHRSMainWin *pMainWin = static_cast<AHRSMainWin *>( parentWidget()->parentWidget() );

    pMainWin->menu();
    update();
}


void AHRSCanvas::swipeRight()
{
    update();
}


void AHRSCanvas::swipeUp()
{
    zoomOut();
    update();
}


void AHRSCanvas::swipeDown()
{
    zoomIn();
    update();
}


void AHRSCanvas::drawDirectOrFromTo( QPainter *pAhrs, CanvasConstants *pC )
{
    if( ((m_directAP.qsID == "NULL") && (m_fromAP.qsID == "NULL")) || (m_airports.count() == 0) )
        return;

    QPen coursePen( Qt::yellow, 8, Qt::SolidLine, Qt::RoundCap );

    if( m_directAP.qsID != "NULL" )
    {
        double  dPxPerNM = static_cast<double>( pC->dW - 30.0 ) / (m_dZoomNM * 2.0);	// Pixels per nautical mile
        QLineF  ball;
        int     iAP = TrafficMath::findAirport( &m_directAP, &m_airports );

        if( iAP >= m_airports.count() )
            return;

        Airport ap = m_airports.at( iAP );

        if( m_bPortrait )
        {
            ball.setP1( QPointF( pC->dW2, pC->dH - pC->dW2 - 30.0 ) );
            ball.setP2( QPointF( pC->dW2, pC->dH - pC->dW2 - 30.0 - (ap.bd.dDistance * dPxPerNM) ) );
        }
        else
        {
            ball.setP1( QPointF( pC->dW + pC->dW2, pC->dH - pC->dW2 - 30.0 ) );
            ball.setP2( QPointF( pC->dW + pC->dW2, pC->dH - pC->dW2 - 30.0 - (ap.bd.dDistance * dPxPerNM) ) );
        }
        if( ball.length() > (pC->dW2 - 30.0) )
            ball.setLength( pC->dW2 - 30.0 );

        // Airport angle in reference to you
        if( g_situation.bHaveWTData )
            ball.setAngle( g_situation.dAHRSMagHeading - ap.bd.dBearing + 90.0 );
        else
            ball.setAngle( g_situation.dAHRSGyroHeading - ap.bd.dBearing + 90.0 ); // - static_cast<double>( m_iMagDev ) );

        pAhrs->setPen( coursePen );
        pAhrs->drawLine( ball );

        double  dDispBearing = ap.bd.dBearing;
        QPixmap num( 320, 84 );

        if( dDispBearing < 0.0 )
            dDispBearing += 360.0;

        Builder::buildNumber( &num, pC, dDispBearing, 0 );
        pAhrs->drawPixmap( pC->dW10 + pC->dW80, pC->dH80 + (m_bPortrait ? 0.0 : pC->dH40), num );
        Builder::buildNumber( &num, pC, ap.bd.dDistance, 1 );
        pAhrs->drawPixmap( pC->dW10 + pC->dW80, pC->dH80 + pC->dH20 + (m_bPortrait ? 0.0 : pC->dH40), num );
    }
    else if( m_fromAP.qsID != "NULL" )
    {
        double  dPxPerNM = static_cast<double>( pC->dW - 30.0 ) / (m_dZoomNM * 2.0);	// Pixels per nautical mile
        QLineF  toLine, fromLine;
        int     iFromAP = TrafficMath::findAirport( &m_fromAP, &m_airports );
        int     iToAP = TrafficMath::findAirport( &m_toAP, &m_airports );

        if( (iFromAP >= m_airports.count()) || (iToAP >= m_airports.count() ) )
            return;

        Airport apFrom = m_airports.at( iFromAP );
        Airport apTo = m_airports.at( iToAP );

        if( m_bPortrait )
        {
            fromLine.setP1( QPointF( pC->dW2, pC->dH - pC->dW2 - 30.0 ) );
            fromLine.setP2( QPointF( pC->dW2, pC->dH - pC->dW2 - 30.0 - (apFrom.bd.dDistance * dPxPerNM) ) );
        }
        else
        {
            fromLine.setP1( QPointF( pC->dW + pC->dW2, pC->dH - pC->dW2 - 30.0 ) );
            fromLine.setP2( QPointF( pC->dW + pC->dW2, pC->dH - pC->dW2 - 30.0 - (apFrom.bd.dDistance * dPxPerNM) ) );
        }
        // Airport angle in reference to you
        if( g_situation.bHaveWTData )
            fromLine.setAngle( g_situation.dAHRSMagHeading - apFrom.bd.dBearing + 90.0 );
        else
            fromLine.setAngle( g_situation.dAHRSGyroHeading - apFrom.bd.dBearing + 90.0 ); // - static_cast<double>( m_iMagDev ) );

        if( m_bPortrait )
        {
            toLine.setP1( QPointF( pC->dW2, pC->dH - pC->dW2 - 30.0 ) );
            toLine.setP2( QPointF( pC->dW2, pC->dH - pC->dW2 - 30.0 - (apTo.bd.dDistance * dPxPerNM) ) );
        }
        else
        {
            toLine.setP1( QPointF( pC->dW + pC->dW2, pC->dH - pC->dW2 - 30.0 ) );
            toLine.setP2( QPointF( pC->dW + pC->dW2, pC->dH - pC->dW2 - 30.0 - (apTo.bd.dDistance * dPxPerNM) ) );
        }

        // Airport angle in reference to you
        if( g_situation.bHaveWTData )
            toLine.setAngle( g_situation.dAHRSMagHeading - apTo.bd.dBearing + 90.0 );
        else
            toLine.setAngle( g_situation.dAHRSGyroHeading - apTo.bd.dBearing + 90.0 ); // - static_cast<double>( m_iMagDev ) );

        QPainterPath maskPath;
        maskPath.addEllipse( 20.0 + (m_bPortrait ? 0 : pC->dW),
                             pC->dH - pC->dW,
                             pC->dW - 40.0, pC->dW - 40.0 );
        pAhrs->setClipPath( maskPath );
        pAhrs->setPen( coursePen );
        pAhrs->drawLine( fromLine.p2(), toLine.p2() );
        pAhrs->setClipping( false );

        double dDispBearing = apTo.bd.dBearing;
        QPixmap num( 320, 84 );

        if( dDispBearing < 0.0 )
            dDispBearing += 360.0;

        Builder::buildNumber( &num, pC, dDispBearing, 0 );
        pAhrs->drawPixmap( pC->dW10 + pC->dW80, pC->dH80 + (m_bPortrait ? 0.0 : pC->dH40), num );
        Builder::buildNumber( &num, pC, apTo.bd.dDistance, 1 );
        pAhrs->drawPixmap( pC->dW10 + pC->dW80, pC->dH80 + pC->dH20 + (m_bPortrait ? 0.0 : pC->dH40), num );
    }
}


void AHRSCanvas::setSwitchableTanks( bool bSwitchable )
{
    m_tanks.bDualTanks = bSwitchable;
    if( !bSwitchable )
    {
        m_tanks.dRightCapacity = 0.0;
        m_tanks.dRightRemaining = 0.0;
    }
}


void AHRSCanvas::setMagDev( int iMagDev )
{
    m_iMagDev = iMagDev;
    update();
}


const QString AHRSCanvas::speedUnits()
{
    QString qsUnits;

    switch( g_eUnitsAirspeed )
    {
        case Canvas::MPH:
            qsUnits = "MPH";
            break;
        case Canvas::Knots:
            qsUnits = "KTS";
            break;
        case Canvas::KPH:
            qsUnits = "KPH";
            break;
    }

    return qsUnits;
}
