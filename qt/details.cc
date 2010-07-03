/*
 * This file Copyright (C) 2009-2010 Mnemosyne LLC
 *
 * This file is licensed by the GPL version 2.  Works owned by the
 * Transmission project are granted a special exemption to clause 2(b)
 * so that the bulk of its code can remain under the MIT license.
 * This exemption does not extend to derived works not owned by
 * the Transmission project.
 *
 * $Id$
 */

#include <cassert>
#include <ctime>
#include <iostream>

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFont>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QResizeEvent>
#include <QSpinBox>
#include <QStyle>
#include <QTabWidget>
#include <QTextBrowser>
#include <QTreeView>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <libtransmission/transmission.h>
#include <libtransmission/bencode.h>

#include "details.h"
#include "file-tree.h"
#include "formatter.h"
#include "hig.h"
#include "prefs.h"
#include "qticonloader.h"
#include "session.h"
#include "squeezelabel.h"
#include "torrent.h"
#include "torrent-model.h"

class Prefs;
class Session;

/****
*****
****/

namespace
{
    const int REFRESH_INTERVAL_MSEC = 4000;

    enum // peer columns
    {
        COL_LOCK,
        COL_UP,
        COL_DOWN,
        COL_PERCENT,
        COL_STATUS,
        COL_ADDRESS,
        COL_CLIENT,
        N_COLUMNS
    };
}

/***
****
***/

class PeerItem: public QTreeWidgetItem
{
        Peer peer;
        QString collatedAddress;
        QString status;

    public:
        virtual ~PeerItem( ) { }
        PeerItem( const Peer& p ) {
            peer = p;
            int q[4];
            if( sscanf( p.address.toUtf8().constData(), "%d.%d.%d.%d", q+0, q+1, q+2, q+3 ) == 4 )
                collatedAddress.sprintf( "%03d.%03d.%03d.%03d", q[0], q[1], q[2], q[3] );
            else
                collatedAddress = p.address;
        }
    public:
        void refresh( const Peer& p ) { peer = p; }
        void setStatus( const QString& s ) { status = s; }
        virtual bool operator< ( const QTreeWidgetItem & other ) const {
            const PeerItem * i = dynamic_cast<const PeerItem*>(&other);
            QTreeWidget * tw( treeWidget( ) );
            const int column = tw ? tw->sortColumn() : 0;
            switch( column ) {
                case COL_UP: return peer.rateToPeer < i->peer.rateToPeer;
                case COL_DOWN: return peer.rateToClient < i->peer.rateToClient;
                case COL_PERCENT: return peer.progress < i->peer.progress;
                case COL_STATUS: return status < i->status;
                case COL_CLIENT: return peer.clientName < i->peer.clientName;
                case COL_LOCK: return peer.isEncrypted && !i->peer.isEncrypted;
                default: return collatedAddress < i->collatedAddress;
            }
        }
};

/***
****
***/

QIcon
Details :: getStockIcon( const QString& freedesktop_name, int fallback )
{
    QIcon fallbackIcon;

    if( fallback > 0 )
        fallbackIcon = style()->standardIcon( QStyle::StandardPixmap( fallback ), 0, this );

    return QtIconLoader::icon( freedesktop_name, fallbackIcon );
}

Details :: Details( Session& session, Prefs& prefs, TorrentModel& model, QWidget * parent ):
    QDialog( parent, Qt::Dialog ),
    mySession( session ),
    myPrefs( prefs ),
    myModel( model ),
    myChangedTorrents( false ),
    myHavePendingRefresh( false )
{
    QVBoxLayout * layout = new QVBoxLayout( this );

    setWindowTitle( tr( "Torrent Properties" ) );

    QTabWidget * t = new QTabWidget( this );
    QWidget * w;
    t->addTab( w = createInfoTab( ),      tr( "Information" ) );
    myWidgets << w;
    t->addTab( w = createPeersTab( ),     tr( "Peers" ) );
    myWidgets << w;
    t->addTab( w = createTrackerTab( ),   tr( "Tracker" ) );
    myWidgets << w;
    t->addTab( w = createFilesTab( ),     tr( "Files" ) );
    myWidgets << w;
    t->addTab( w = createOptionsTab( ),   tr( "Options" ) );
    myWidgets << w;
    layout->addWidget( t );

    QDialogButtonBox * buttons = new QDialogButtonBox( QDialogButtonBox::Close, Qt::Horizontal, this );
    connect( buttons, SIGNAL(rejected()), this, SLOT(close()));
    layout->addWidget( buttons );
    QWidget::setAttribute( Qt::WA_DeleteOnClose, true );

    connect( &myTimer, SIGNAL(timeout()), this, SLOT(onTimer()));

    onTimer( );
    myTimer.setSingleShot( false );
    myTimer.start( REFRESH_INTERVAL_MSEC );
}

Details :: ~Details( )
{
}

void
Details :: setIds( const QSet<int>& ids )
{
    if( ids == myIds )
        return;

    myChangedTorrents = true;

    // stop listening to the old torrents
    foreach( int id, myIds ) {
        const Torrent * tor = myModel.getTorrentFromId( id );
        if( tor )
            disconnect( tor, SIGNAL(torrentChanged(int)), this, SLOT(onTorrentChanged()) );
    }

    myFileTreeView->clear( );

    myIds = ids;

    // listen to the new torrents
    foreach( int id, myIds ) {
        const Torrent * tor = myModel.getTorrentFromId( id );
        if( tor )
            connect( tor, SIGNAL(torrentChanged(int)), this, SLOT(onTorrentChanged()) );
    }

    foreach( QWidget * w, myWidgets )
        w->setEnabled( false );

    onTimer( );
}

/***
****
***/

QString
Details :: timeToStringRounded( int seconds )
{
    if( seconds > 60 ) seconds -= ( seconds % 60 );
    return Formatter::timeToString ( seconds );
}

void
Details :: onTimer( )
{
    if( !myIds.empty( ) )
    {
        QSet<int> infos;
        foreach( int id, myIds ) {
            const Torrent * tor = myModel.getTorrentFromId( id );
            if( tor->isMagnet() )
                infos.insert( tor->id() );
        }
        if( !infos.isEmpty() )
            mySession.initTorrents( infos );
        mySession.refreshExtraStats( myIds );
    }
}

void
Details :: onTorrentChanged( )
{
    if( !myHavePendingRefresh ) {
        myHavePendingRefresh = true;
        QTimer::singleShot( 100, this, SLOT(refresh()));
    }
}


void
Details :: refresh( )
{
    const int n = myIds.size( );
    const bool single = n == 1;
    const QString blank;
    const QFontMetrics fm( fontMetrics( ) );
    QList<const Torrent*> torrents;
    QString string;
    const QString none = tr( "None" );
    const QString mixed = tr( "Mixed" );
    const QString unknown = tr( "Unknown" );

    // build a list of torrents
    foreach( int id, myIds ) {
        const Torrent * tor = myModel.getTorrentFromId( id );
        if( tor )
            torrents << tor;
    }

    ///
    ///  activity tab
    ///

    // myStateLabel
    if( torrents.empty( ) )
        string = none;
    else {
        bool isMixed = false;
        bool allPaused = true;
        bool allFinished = true;
        const tr_torrent_activity activity = torrents[0]->getActivity( );
        foreach( const Torrent * t, torrents ) {
            if( activity != t->getActivity( ) )
                isMixed = true;
            if( activity != TR_STATUS_STOPPED )
                allPaused = allFinished = false;
            if( !t->isFinished( ) )
                allFinished = false;
        }
        if( isMixed )
            string = mixed;
        else if( allFinished )
            string = tr( "Finished" );
        else if( allPaused )
            string = tr( "Paused" );
        else
            string = torrents[0]->activityString( );
    }
    myStateLabel->setText( string );
    const QString stateString = string;

    // myHaveLabel
    double sizeWhenDone = 0;
    double leftUntilDone = 0;
    double available = 0;
    int64_t haveTotal = 0;
    int64_t haveVerified = 0;
    int64_t haveUnverified = 0;
    int64_t verifiedPieces = 0;
    if( torrents.empty( ) )
        string = none;
    else {
        foreach( const Torrent * t, torrents ) {
            if( t->hasMetadata( ) ) {
                haveTotal += t->haveTotal( );
                haveUnverified += t->haveUnverified( );
                const uint64_t v = t->haveVerified( );
                haveVerified += v;
                verifiedPieces += v / t->pieceSize( );
                sizeWhenDone += t->sizeWhenDone( );
                leftUntilDone += t->leftUntilDone( );
                available += t->sizeWhenDone() - t->leftUntilDone() + t->desiredAvailable();
            }
        }
        if( !haveVerified && !haveUnverified )
            string = none;
        else {
            const double d = 100.0 * ( sizeWhenDone ? ( sizeWhenDone - leftUntilDone ) / sizeWhenDone : 1 );
            QString pct = Formatter::percentToString( d );
            if( !haveUnverified )
                string = tr( "%1 (%2%)" )
                             .arg( Formatter::sizeToString( haveVerified + haveUnverified ) )
                             .arg( pct );
            else
                string = tr( "%1 (%2%); %3 Unverified" )
                             .arg( Formatter::sizeToString( haveVerified + haveUnverified ) )
                             .arg( pct )
                             .arg( Formatter::sizeToString( haveUnverified ) );
        }
    }
    myHaveLabel->setText( string );

    // myAvailabilityLabel
    if( torrents.empty( ) )
        string = none;
    else {
        if( sizeWhenDone == 0 )
            string = none;
        else
            string = QString( "%1%" ).arg( Formatter::percentToString( ( 100.0 * available ) / sizeWhenDone ) );
    }
    myAvailabilityLabel->setText( string );

    // myDownloadedLabel
    uint64_t d = 0, f = 0;
    if( torrents.empty( ) )
        string = none;
    else {
        foreach( const Torrent * t, torrents ) {
            d += t->downloadedEver( );
            f += t->failedEver( );
        }
        const QString dstr = Formatter::sizeToString( d );
        const QString fstr = Formatter::sizeToString( f );
        if( f )
            string = tr( "%1 (+%2 corrupt)" ).arg( dstr ).arg( fstr );
        else
            string = dstr;
    }
    myDownloadedLabel->setText( string );

    uint64_t u = 0;
    if( torrents.empty( ) )
        string = none;
    else {
        foreach( const Torrent * t, torrents ) u += t->uploadedEver( );
        string = QString( Formatter::sizeToString( u ) );
    }
    myUploadedLabel->setText( string );

    if( torrents.empty( ) )
        string = none;
    else if( torrents.length() == 1 )
        string = Formatter::ratioToString( torrents.first()->ratio() );
    else {
        bool isMixed = false;
        int ratioType = torrents.first()->ratio();
        if( ratioType > 0 ) ratioType = 0;
        foreach( const Torrent *t, torrents )
        {
            if( ratioType != ( t->ratio() >= 0 ? 0 : t->ratio() ) )
            {
                isMixed = true;
                break;
            }
        }
        if( isMixed )
            string = mixed;
        else if( ratioType < 0 )
            string = Formatter::ratioToString( ratioType );
        else
            string = Formatter::ratioToString( (double)u / d );
    }
    myRatioLabel->setText( string );

    const QDateTime qdt_now = QDateTime::currentDateTime( );

    // myRunTimeLabel
    if( torrents.empty( ) )
        string = none;
    else {
        bool allPaused = true;
        QDateTime baseline = torrents[0]->lastStarted( );
        foreach( const Torrent * t, torrents ) {
            if( baseline != t->lastStarted( ) )
                baseline = QDateTime( );
            if( !t->isPaused( ) )
                allPaused = false;
        }
        if( allPaused )
            string = stateString; // paused || finished
        else if( baseline.isNull( ) )
            string = mixed;
        else
            string = Formatter::timeToString( baseline.secsTo( qdt_now ) );
    }
    myRunTimeLabel->setText( string );


    // myETALabel
    string.clear( );
    if( torrents.empty( ) )
        string = none;
    else {
        int baseline = torrents[0]->getETA( );
        foreach( const Torrent * t, torrents ) {
            if( baseline != t->getETA( ) ) {
                string = mixed;
                break;
            }
        }
        if( string.isEmpty( ) ) {
            if( baseline < 0 )
                string = tr( "Unknown" );
            else
                string = Formatter::timeToString( baseline );
       }
    }
    myETALabel->setText( string );


    // myLastActivityLabel
    if( torrents.empty( ) )
        string = none;
    else {
        QDateTime latest = torrents[0]->lastActivity( );
        foreach( const Torrent * t, torrents ) {
            const QDateTime dt = t->lastActivity( );
            if( latest < dt )
                latest = dt;
        }
        const int seconds = latest.secsTo( qdt_now );
        if( seconds < 5 )
            string = tr( "Active now" );
        else
            string = tr( "%1 ago" ).arg( Formatter::timeToString( seconds ) );
    }
    myLastActivityLabel->setText( string );


    if( torrents.empty( ) )
        string = none;
    else {
        string = torrents[0]->getError( );
        foreach( const Torrent * t, torrents ) {
            if( string != t->getError( ) ) {
                string = mixed;
                break;
            }
        }
    }
    if( string.isEmpty( ) )
        string = none;
    myErrorLabel->setText( string );


    ///
    /// information tab
    ///

    // mySizeLabel
    if( torrents.empty( ) )
        string = none;
    else {
        int pieces = 0;
        uint64_t size = 0;
        uint32_t pieceSize = torrents[0]->pieceSize( );
        foreach( const Torrent * t, torrents ) {
            pieces += t->pieceCount( );
            size += t->totalSize( );
            if( pieceSize != t->pieceSize( ) )
                pieceSize = 0;
        }
        if( !size )
            string = none;
        else if( pieceSize > 0 )
            string = tr( "%1 (%Ln pieces @ %2)", "", pieces )
                     .arg( Formatter::sizeToString( size ) )
                     .arg( Formatter::memToString( pieceSize ) );
        else
            string = tr( "%1 (%Ln pieces)", "", pieces )
                     .arg( Formatter::sizeToString( size ) );
    }
    mySizeLabel->setText( string );

    // myHashLabel
    if( torrents.empty( ) )
        string = none;
    else {
        string = torrents[0]->hashString( );
        foreach( const Torrent * t, torrents ) {
            if( string != t->hashString( ) ) {
                string = mixed;
                break;
            }
        }
    }
    myHashLabel->setText( string );

    // myPrivacyLabel
    if( torrents.empty( ) )
        string = none;
    else {
        bool b = torrents[0]->isPrivate( );
        string = b ? tr( "Private to this tracker -- DHT and PEX disabled" )
                   : tr( "Public torrent" );
        foreach( const Torrent * t, torrents ) {
            if( b != t->isPrivate( ) ) {
                string = mixed;
                break;
            }
        }
    }
    myPrivacyLabel->setText( string );

    // myCommentBrowser
    if( torrents.empty( ) )
        string = none;
    else {
        string = torrents[0]->comment( );
        foreach( const Torrent * t, torrents ) {
            if( string != t->comment( ) ) {
                string = mixed;
                break;
            }
        }
    }
    myCommentBrowser->setText( string );
    myCommentBrowser->setMaximumHeight( QWIDGETSIZE_MAX );

    // myOriginLabel
    if( torrents.empty( ) )
        string = none;
    else {
        bool mixed_creator=false, mixed_date=false;
        const QString creator = torrents[0]->creator();
        const QString date = torrents[0]->dateCreated().toString();
        foreach( const Torrent * t, torrents ) {
            mixed_creator |= ( creator != t->creator() );
            mixed_date |=  ( date != t->dateCreated().toString() );
        }
        if( mixed_creator && mixed_date )
            string = mixed;
        else if( mixed_date )
            string = tr( "Created by %1" ).arg( creator );
        else if( mixed_creator || creator.isEmpty( ) )
            string = tr( "Created on %1" ).arg( date );
        else
            string = tr( "Created by %1 on %2" ).arg( creator ).arg( date );
    }
    myOriginLabel->setText( string );

    // myLocationLabel
    if( torrents.empty( ) )
        string = none;
    else {
        string = torrents[0]->getPath( );
        foreach( const Torrent * t, torrents ) {
            if( string != t->getPath( ) ) {
                string = mixed;
                break;
            }
        }
    }
    myLocationLabel->setText( string );


    ///
    ///  Options Tab
    ///

    if( myChangedTorrents && !torrents.empty( ) )
    {
        int i;
        const Torrent * baseline = *torrents.begin();
        const Torrent * tor;
        bool uniform;
        bool baselineFlag;
        int baselineInt;

        // mySessionLimitCheck
        uniform = true;
        baselineFlag = baseline->honorsSessionLimits( );
        foreach( tor, torrents ) if( baselineFlag != tor->honorsSessionLimits( ) ) { uniform = false; break; }
        mySessionLimitCheck->setChecked( uniform && baselineFlag );

        // mySingleDownCheck
        uniform = true;
        baselineFlag = baseline->downloadIsLimited( );
        foreach( tor, torrents ) if( baselineFlag != tor->downloadIsLimited( ) ) { uniform = false; break; }
        mySingleDownCheck->setChecked( uniform && baselineFlag );

        // mySingleUpCheck
        uniform = true;
        baselineFlag = baseline->uploadIsLimited( );
        foreach( tor, torrents ) if( baselineFlag != tor->uploadIsLimited( ) ) { uniform = false; break; }
        mySingleUpCheck->setChecked( uniform && baselineFlag );

        // myBandwidthPriorityCombo
        uniform = true;
        baselineInt = baseline->getBandwidthPriority( );
        foreach( tor, torrents ) if ( baselineInt != tor->getBandwidthPriority( ) ) { uniform = false; break; }
        if( uniform )
            i = myBandwidthPriorityCombo->findData( baselineInt );
        else
            i = -1;
        myBandwidthPriorityCombo->blockSignals( true );
        myBandwidthPriorityCombo->setCurrentIndex( i );
        myBandwidthPriorityCombo->blockSignals( false );

        mySingleDownSpin->blockSignals( true );
        mySingleDownSpin->setValue( tor->downloadLimit().Bps() / Formatter::speed_K );
        mySingleDownSpin->blockSignals( false );

        mySingleUpSpin->blockSignals( true );
        mySingleUpSpin->setValue( tor->uploadLimit().Bps() / Formatter::speed_K );
        mySingleUpSpin->blockSignals( false );

        myPeerLimitSpin->blockSignals( true );
        myPeerLimitSpin->setValue( tor->peerLimit() );
        myPeerLimitSpin->blockSignals( false );

        // ratio radios
        uniform = true;
        baselineInt = tor->seedRatioMode( );
        foreach( tor, torrents ) if( baselineInt != tor->seedRatioMode( ) ) { uniform = false; break; }
        if( !uniform ) {
            mySeedGlobalRadio->setChecked( false );
            mySeedCustomRadio->setChecked( false );
            mySeedForeverRadio->setChecked( false );
        } else {
            QRadioButton * rb;
            switch( baselineInt ) {
                case TR_RATIOLIMIT_GLOBAL:    rb = mySeedGlobalRadio; break;
                case TR_RATIOLIMIT_SINGLE:    rb = mySeedCustomRadio; break;
                case TR_RATIOLIMIT_UNLIMITED: rb = mySeedForeverRadio; break;
            }
            rb->setChecked( true );
        }

        mySeedCustomSpin->blockSignals( true );
        mySeedCustomSpin->setValue( tor->seedRatioLimit( ) );
        mySeedCustomSpin->blockSignals( false );
    }

    // tracker tab
    //
    QMap<QString,QTreeWidgetItem*> trackerTiers;
    QMap<QString,QTreeWidgetItem*> trackerItems;
    const time_t now( time( 0 ) );
    const bool showScrape = myPrefs.getBool( Prefs::SHOW_TRACKER_SCRAPES );
    foreach( const Torrent * t, torrents )
    {
        const QString idStr( QString::number( t->id( ) ) );
        const TrackerStatsList trackerStats = t->trackerStats( );

        foreach( const TrackerStat& trackerStat, trackerStats )
        {
            QFont font;
            QString str;
            const QString tierKey( QString::number(trackerStat.tier) );
            QTreeWidgetItem * tier = (QTreeWidgetItem*) myTrackerTiers.value( tierKey, 0 );

            if( tier == 0 ) // new tier
            {
                QFont tierFont;
                tier = new QTreeWidgetItem( myTrackerTree );
                myTrackerTree->addTopLevelItem( tier );
                str = "Tier: " + QString::number( trackerStat.tier + 1 );
                tier->setText( 0, str );
                tierFont.setBold( true );
                tier->setFont( 0, tierFont );
            }

            const QString key( idStr + tierKey + ":" + QString::number( trackerStat.id ) );
            QTreeWidgetItem * item = (QTreeWidgetItem*) myTrackerItems.value( key, 0 );

            if( item == 0 ) // new tracker
            {
                item = new QTreeWidgetItem( tier );
                tier->addChild( item );
                if( tier->childCount() == 1 )
                    tier->setExpanded( true );
            }
            str = trackerStat.host;

            if( trackerStat.isBackup )
            {
                font.setItalic( true );
                if( showScrape )
                {
                    str += "\n";
                    str += "Tracker will be used as a backup";
                }
            }
            else
            {
                font.setItalic( false );
                if( trackerStat.hasAnnounced )
                {
                    const QString tstr( timeToStringRounded( now - trackerStat.lastAnnounceTime ) );
                    str += "\n";
                    if( trackerStat.lastAnnounceSucceeded )
                    {
                        str += tr( "Got a list of %1 peers %2 ago" )
                            .arg( trackerStat.lastAnnouncePeerCount )
                            .arg( tstr );
                    }
                    else if( trackerStat.lastAnnounceTimedOut )
                    {
                        str += tr( "Peer list request timed out %1 ago; will retry" )
                            .arg( tstr );
                    }
                    else
                    {
                        str += tr( "Got an error %1 ago" )
                            .arg( tstr );
                    }
                }
                switch( trackerStat.announceState )
                {
                    case TR_TRACKER_INACTIVE:
                        if( trackerStat.hasAnnounced )
                        {
                            str += "\n";
                            str += tr( "No updates scheduled" );
                        }
                        break;
                    case TR_TRACKER_WAITING:
                        {
                            const QString tstr( timeToStringRounded( trackerStat.nextAnnounceTime - now ) );
                            str += "\n";
                            str += tr( "Asking for more peers in %1" )
                                .arg( tstr );
                        }
                        break;
                    case TR_TRACKER_QUEUED:
                        str += "\n";
                        str += tr( "Queued to ask for more peers" );
                        break;
                    case TR_TRACKER_ACTIVE:
                        {
                            const QString tstr( timeToStringRounded( now - trackerStat.lastAnnounceStartTime ) );
                            str += "\n";
                            str += tr( "Asking for more peers now... %1" )
                                .arg( tstr );
                        }
                        break;
                }
                if( showScrape )
                {
                    if( trackerStat.hasScraped )
                    {
                        const QString tstr( timeToStringRounded( now - trackerStat.lastScrapeTime ) );
                        str += "\n";
                        if( trackerStat.lastScrapeSucceeded )
                        {
                            str += tr( "Tracker had %1 seeders and %2 leechers %3 ago" )
                                .arg( trackerStat.seederCount )
                                .arg( trackerStat.leecherCount )
                                .arg( tstr );
                        }
                        else
                        {
                            str += tr( "Got a scrape error %1 ago" )
                                .arg( tstr );
                        }
                    }
                    switch( trackerStat.scrapeState )
                    {
                        case TR_TRACKER_INACTIVE:
                            break;
                        case TR_TRACKER_WAITING:
                            {
                                const QString tstr( timeToStringRounded( trackerStat.nextScrapeTime - now ) );
                                str += "\n";
                                str += tr( "Asking for peer counts in %1" )
                                    .arg( tstr );
                            }
                            break;
                        case TR_TRACKER_QUEUED:
                            str += "\n";
                            str += tr( "Queued to ask for peer counts" );
                            break;
                        case TR_TRACKER_ACTIVE:
                            {
                                const QString tstr( timeToStringRounded( now - trackerStat.lastScrapeStartTime ) );
                                str += "\n";
                                str += tr( "Asking for peer counts now... %1" )
                                    .arg( tstr );
                            }
                            break;
                    }
                }
            }
            item->setText( 0, str );
            item->setFont( 0, font );
            item->setData( 0, TRACKERID, trackerStat.id );
            item->setData( 0, TRACKERURL, trackerStat.announce );
            item->setData( 0, TRACKERTIER, trackerStat.tier );
            item->setData( 0, TORRENTID, t->id() );

            tier->setData( 0, TRACKERID, -1 );
            tier->setData( 0, TRACKERURL, QString() );
            tier->setData( 0, TRACKERTIER, trackerStat.tier );
            tier->setData( 0, TORRENTID, torrents.count() > 1 ? -1 : t->id() );

            trackerTiers.insert( tierKey, tier );
            trackerItems.insert( key, item );
        }
    }
    QList<QTreeWidgetItem*> tierList = trackerTiers.values();
    QList<QTreeWidgetItem*> itemList = trackerItems.values();
    for( int i = 0; i < myTrackerTree->topLevelItemCount(); ++i )
    {
        QTreeWidgetItem * tier = myTrackerTree->topLevelItem( i );
        for( int j = 0; j < tier->childCount(); ++j )
        {
            if( !itemList.contains( tier->child( j ) ) ) // tracker has disappeared
                delete tier->takeChild( j-- );
        }
        if( !tierList.contains( tier ) ) // tier has disappeared
            delete myTrackerTree->takeTopLevelItem( i-- );
    }
    myTrackerTiers = trackerTiers;
    myTrackerItems = trackerItems;

    ///
    ///  Peers tab
    ///

    QMap<QString,QTreeWidgetItem*> peers2;
    QList<QTreeWidgetItem*> newItems;
    foreach( const Torrent * t, torrents )
    {
        const QString idStr( QString::number( t->id( ) ) );
        PeerList peers = t->peers( );

        foreach( const Peer& peer, peers )
        {
            const QString key = idStr + ":" + peer.address;
            PeerItem * item = (PeerItem*) myPeers.value( key, 0 );

            if( item == 0 ) // new peer has connected
            {
                static const QIcon myEncryptionIcon( ":/icons/encrypted.png" );
                static const QIcon myEmptyIcon;
                item = new PeerItem( peer );
                item->setTextAlignment( COL_UP, Qt::AlignRight );
                item->setTextAlignment( COL_DOWN, Qt::AlignRight );
                item->setTextAlignment( COL_PERCENT, Qt::AlignRight );
                item->setIcon( COL_LOCK, peer.isEncrypted ? myEncryptionIcon : myEmptyIcon );
                item->setToolTip( COL_LOCK, peer.isEncrypted ? tr( "Encrypted connection" ) : "" );
                item->setText( COL_ADDRESS, peer.address );
                item->setText( COL_CLIENT, peer.clientName );
                newItems << item;
            }

            const QString code = peer.flagStr;
            item->setStatus( code );
            item->refresh( peer );

            QString codeTip;
            foreach( QChar ch, code ) {
                QString txt;
                switch( ch.toAscii() ) {
                    case 'O': txt = tr( "Optimistic unchoke" ); break;
                    case 'D': txt = tr( "Downloading from this peer" ); break;
                    case 'd': txt = tr( "We would download from this peer if they would let us" ); break;
                    case 'U': txt = tr( "Uploading to peer" ); break;
                    case 'u': txt = tr( "We would upload to this peer if they asked" ); break;
                    case 'K': txt = tr( "Peer has unchoked us, but we're not interested" ); break;
                    case '?': txt = tr( "We unchoked this peer, but they're not interested" ); break;
                    case 'E': txt = tr( "Encrypted connection" ); break;
                    case 'H': txt = tr( "Peer was discovered through DHT" ); break;
                    case 'X': txt = tr( "Peer was discovered through Peer Exchange (PEX)" ); break;
                    case 'I': txt = tr( "Peer is an incoming connection" ); break;
                }
                if( !txt.isEmpty( ) )
                    codeTip += QString("%1: %2\n").arg(ch).arg(txt);
            }

            if( !codeTip.isEmpty() )
                codeTip.resize( codeTip.size()-1 ); // eat the trailing linefeed

            item->setText( COL_UP, peer.rateToPeer.isZero() ? "" : Formatter::speedToString( peer.rateToPeer ) );
            item->setText( COL_DOWN, peer.rateToClient.isZero() ? "" : Formatter::speedToString( peer.rateToClient ) );
            item->setText( COL_PERCENT, peer.progress > 0 ? QString( "%1%" ).arg( (int)( peer.progress * 100.0 ) ) : "" );
            item->setText( COL_STATUS, code );
            item->setToolTip( COL_STATUS, codeTip );

            peers2.insert( key, item );
        }
    }
    myPeerTree->addTopLevelItems( newItems );
    foreach( QString key, myPeers.keys() ) {
        if( !peers2.contains( key ) ) { // old peer has disconnected
            QTreeWidgetItem * item = myPeers.value( key, 0 );
            myPeerTree->takeTopLevelItem( myPeerTree->indexOfTopLevelItem( item ) );
            delete item;
        }
    }
    myPeers = peers2;

    if( single )
        myFileTreeView->update( torrents[0]->files( ) , myChangedTorrents );
    else
        myFileTreeView->clear( );

    myChangedTorrents = false;
    myHavePendingRefresh = false;
    foreach( QWidget * w, myWidgets )
        w->setEnabled( true );
}

void
Details :: enableWhenChecked( QCheckBox * box, QWidget * w )
{
    connect( box, SIGNAL(toggled(bool)), w, SLOT(setEnabled(bool)) );
    w->setEnabled( box->isChecked( ) );
}


/***
****
***/

QWidget *
Details :: createInfoTab( )
{
    HIG * hig = new HIG( this );

    hig->addSectionTitle( tr( "Activity" ) );
    hig->addRow( tr( "Torrent size:" ), mySizeLabel = new SqueezeLabel );
    hig->addRow( tr( "Have:" ), myHaveLabel = new SqueezeLabel );
    hig->addRow( tr( "Availability:" ), myAvailabilityLabel = new SqueezeLabel );
    hig->addRow( tr( "Downloaded:" ), myDownloadedLabel = new SqueezeLabel );
    hig->addRow( tr( "Uploaded:" ), myUploadedLabel = new SqueezeLabel );
    hig->addRow( tr( "Ratio:" ), myRatioLabel = new SqueezeLabel );
    hig->addRow( tr( "State:" ), myStateLabel = new SqueezeLabel );
    hig->addRow( tr( "Running time:" ), myRunTimeLabel = new SqueezeLabel );
    hig->addRow( tr( "Remaining time:" ), myETALabel = new SqueezeLabel );
    hig->addRow( tr( "Last activity:" ), myLastActivityLabel = new SqueezeLabel );
    hig->addRow( tr( "Error:" ), myErrorLabel = new SqueezeLabel );
    hig->addSectionDivider( );

    hig->addSectionDivider( );
    hig->addSectionTitle( tr( "Details" ) );
    hig->addRow( tr( "Location:" ), myLocationLabel = new SqueezeLabel );
    hig->addRow( tr( "Hash:" ), myHashLabel = new SqueezeLabel );
    hig->addRow( tr( "Privacy:" ), myPrivacyLabel = new SqueezeLabel );
    hig->addRow( tr( "Origin:" ), myOriginLabel = new SqueezeLabel );
    myOriginLabel->setMinimumWidth( 325 ); // stop long origin strings from resizing the widgit
    hig->addRow( tr( "Comment:" ), myCommentBrowser = new QTextBrowser );
    const int h = QFontMetrics(myCommentBrowser->font()).lineSpacing() * 4;
    myCommentBrowser->setFixedHeight( h );

    hig->finish( );

    return hig;
}

/***
****
***/

void
Details :: onShowTrackerScrapesToggled( bool val )
{
    myPrefs.set( Prefs::SHOW_TRACKER_SCRAPES, val );
}

void
Details :: onHonorsSessionLimitsToggled( bool val )
{
    mySession.torrentSet( myIds, "honorsSessionLimits", val );
}
void
Details :: onDownloadLimitedToggled( bool val )
{
    mySession.torrentSet( myIds, "downloadLimited", val );
}
void
Details :: onDownloadLimitChanged( int val )
{
    mySession.torrentSet( myIds, "downloadLimit", val * Formatter::speed_K  );
}
void
Details :: onUploadLimitedToggled( bool val )
{
    mySession.torrentSet( myIds, "uploadLimited", val );
}
void
Details :: onUploadLimitChanged( int val )
{
    mySession.torrentSet( myIds, "uploadLimit", val * Formatter::speed_K );
}

#define RATIO_KEY "seedRatioMode"

void
Details :: onSeedUntilChanged( bool b )
{
    if( b )
        mySession.torrentSet( myIds, RATIO_KEY, sender()->property(RATIO_KEY).toInt() );
}

void
Details :: onSeedRatioLimitChanged( double val )
{
    QSet<int> ids;

    foreach( int id, myIds ) {
        const Torrent * tor = myModel.getTorrentFromId( id );
        if( tor && tor->seedRatioLimit( ) )
            ids.insert( id );
    }

    if( !ids.empty( ) )
        mySession.torrentSet( ids, "seedRatioLimit", val );
}

void
Details :: onMaxPeersChanged( int val )
{
    mySession.torrentSet( myIds, "peer-limit", val );
}

void
Details :: onBandwidthPriorityChanged( int index )
{
    if( index != -1 )
    {
        const int priority = myBandwidthPriorityCombo->itemData(index).toInt( );
        mySession.torrentSet( myIds, "bandwidthPriority", priority );
    }
}

void
Details :: onTrackerSelectionChanged( )
{
    const QList<QTreeWidgetItem*> items = myTrackerTree->selectedItems();
    if( items.count() == 1 )
        myEditTrackerButton->setEnabled( items.first()->data( 0, TRACKERID ).toInt() >= 0 );
    else
        myEditTrackerButton->setEnabled( false );
    myRemoveTrackerButton->setEnabled( !items.isEmpty() );
}

bool
Details :: findTrackerByURL( const QString& url, int torId )
{
    bool duplicate = false;
    foreach( QTreeWidgetItem * tracker, myTrackerItems.values() )
    {
        if( tracker->data( 0, TRACKERURL ).toString() == url &&
          ( torId == -1 || tracker->data( 0, TORRENTID ).toInt() == torId ) )
        {
            duplicate = true;
            break;
        }
    }
    return duplicate;
}

void
Details :: onAddTrackerPushed( )
{
    const QString urlString = QInputDialog::getText( this,
                                                     tr( "Add tracker announce URL " ),
                                                     NULL );
    if( !urlString.isEmpty() )
    {
        if( !findTrackerByURL( urlString, -1 ) )
        {
            QByteArray url = urlString.toUtf8();
            tr_benc top;

            tr_bencInitDict( &top, 1 );
            tr_bencDictAddStr( &top, "announce", url );

            mySession.torrentSet( myIds, "trackerAdd", &top );
        }
        else
            QMessageBox::warning( this, "Error", "Tracker already exists." );
    }
}

void
Details :: onEditTrackerPushed( )
{
    const QTreeWidgetItem * item = myTrackerTree->selectedItems().first();
    const QString urlString = QInputDialog::getText( this,
                                                     tr( "Edit tracker announce URL " ),
                                                     NULL,
                                                     QLineEdit::Normal,
                                                     item->data( 0, TRACKERURL ).toString() );
    if( !urlString.isEmpty() )
    {
        const int torId = item->data( 0, TORRENTID ).toInt();
        if( !findTrackerByURL( urlString, torId ) )
        {
            QByteArray url = urlString.toUtf8();
            QSet<int> ids;
            tr_benc top;

            ids << torId;
            tr_bencInitDict( &top, 2 );
            tr_bencDictAddStr( &top, "announce", item->data( 0, TRACKERURL ).toByteArray() );
            tr_bencDictAddStr( &top, "announce-new", url );

            mySession.torrentSet( ids, "trackerEdit", &top );
        }
        else
            QMessageBox::warning( this, "Error", "Tracker already exists." );
    }
}

void
Details :: removeTracker( const QTreeWidgetItem * item )
{
    QByteArray url = item->data( 0, TRACKERURL ).toByteArray();
    const int torId = item->data( 0, TORRENTID ).toInt();
    QSet<int> ids;
    tr_benc top;

    ids << torId;
    tr_bencInitDict( &top, 1 );
    tr_bencDictAddStr( &top, "announce", url );

    mySession.torrentSet( ids, "trackerRemove", &top );
}

void
Details :: onRemoveTrackerPushed( )
{
    const QList<QTreeWidgetItem*> items = myTrackerTree->selectedItems();
    QSet<int> removedTiers;
    foreach( const QTreeWidgetItem * item, items ) {
        const bool isTier = item->data( 0, TRACKERID ).toInt() == -1;
        const int curTier = item->data( 0, TRACKERTIER ).toInt();
        if( isTier )
        {
            removedTiers << curTier;
            for( int i = 0; i < item->childCount(); ++i )
                removeTracker( item->child( i ) );
        }
        else if( !removedTiers.contains( curTier ) ) // skip trackers removed by clearing a tier
            removeTracker( item );
    }
}

QWidget *
Details :: createOptionsTab( )
{
    //QWidget * l;
    QSpinBox * s;
    QCheckBox * c;
    QComboBox * m;
    QHBoxLayout * h;
    QRadioButton * r;
    QDoubleSpinBox * ds;

    HIG * hig = new HIG( this );
    hig->addSectionTitle( tr( "Speed" ) );

    c = new QCheckBox( tr( "Honor global &limits" ) );
    mySessionLimitCheck = c;
    hig->addWideControl( c );
    connect( c, SIGNAL(clicked(bool)), this, SLOT(onHonorsSessionLimitsToggled(bool)) );

    c = new QCheckBox( tr( "Limit &download speed (%1):" ).arg( Formatter::speed_K_str ) );
    mySingleDownCheck = c;
    s = new QSpinBox( );
    mySingleDownSpin = s;
    s->setRange( 0, INT_MAX );
    hig->addRow( c, s );
    enableWhenChecked( c, s );
    connect( c, SIGNAL(clicked(bool)), this, SLOT(onDownloadLimitedToggled(bool)) );
    connect( s, SIGNAL(valueChanged(int)), this, SLOT(onDownloadLimitChanged(int)));

    c = new QCheckBox( tr( "Limit &upload speed (%1):" ).arg( Formatter::speed_K_str ) );
    mySingleUpCheck = c;
    s = new QSpinBox( );
    mySingleUpSpin = s;
    s->setRange( 0, INT_MAX );
    hig->addRow( c, s );
    enableWhenChecked( c, s );
    connect( c, SIGNAL(clicked(bool)), this, SLOT(onUploadLimitedToggled(bool)) );
    connect( s, SIGNAL(valueChanged(int)), this, SLOT(onUploadLimitChanged(int)));

    m = new QComboBox;
    m->addItem( tr( "High" ),   TR_PRI_HIGH );
    m->addItem( tr( "Normal" ), TR_PRI_NORMAL );
    m->addItem( tr( "Low" ),    TR_PRI_LOW );
    connect( m, SIGNAL(currentIndexChanged(int)), this, SLOT(onBandwidthPriorityChanged(int)));
    hig->addRow( tr( "Torrent &priority:" ), m );
    myBandwidthPriorityCombo = m;

    hig->addSectionDivider( );
    hig->addSectionTitle( tr( "Seed-Until Ratio" ) );

    r = new QRadioButton( tr( "Use &global settings" ) );
    r->setProperty( RATIO_KEY, TR_RATIOLIMIT_GLOBAL );
    connect( r, SIGNAL(clicked(bool)), this, SLOT(onSeedUntilChanged(bool)));
    mySeedGlobalRadio = r;
    hig->addWideControl( r );

    r = new QRadioButton( tr( "Seed &regardless of ratio" ) );
    r->setProperty( RATIO_KEY, TR_RATIOLIMIT_UNLIMITED );
    connect( r, SIGNAL(clicked(bool)), this, SLOT(onSeedUntilChanged(bool)));
    mySeedForeverRadio = r;
    hig->addWideControl( r );

    h = new QHBoxLayout( );
    h->setSpacing( HIG :: PAD );
    r = new QRadioButton( tr( "&Seed torrent until its ratio reaches:" ) );
    r->setProperty( RATIO_KEY, TR_RATIOLIMIT_SINGLE );
    connect( r, SIGNAL(clicked(bool)), this, SLOT(onSeedUntilChanged(bool)));
    mySeedCustomRadio = r;
    h->addWidget( r );
    ds = new QDoubleSpinBox( );
    ds->setRange( 0.5, INT_MAX );
    connect( ds, SIGNAL(valueChanged(double)), this, SLOT(onSeedRatioLimitChanged(double)));
    mySeedCustomSpin = ds;
    h->addWidget( ds );
    hig->addWideControl( h );

    hig->addSectionDivider( );
    hig->addSectionTitle( tr( "Peer Connections" ) );

    s = new QSpinBox( );
    s->setRange( 1, 300 );
    connect( s, SIGNAL(valueChanged(int)), this, SLOT(onMaxPeersChanged(int)));
    myPeerLimitSpin = s;
    hig->addRow( tr( "&Maximum peers:" ), s );

    hig->finish( );

    return hig;
}

/***
****
***/

QWidget *
Details :: createTrackerTab( )
{
    QCheckBox * c;
    QPushButton * p;
    QWidget * top = new QWidget;
    QVBoxLayout * v = new QVBoxLayout( top );
    QHBoxLayout * h = new QHBoxLayout();
    QVBoxLayout * v2 = new QVBoxLayout();

    v->setSpacing( HIG::PAD_BIG );
    v->setContentsMargins( HIG::PAD_BIG, HIG::PAD_BIG, HIG::PAD_BIG, HIG::PAD_BIG );

    h->setSpacing( HIG::PAD );
    h->setContentsMargins( HIG::PAD_SMALL, HIG::PAD_SMALL, HIG::PAD_SMALL, HIG::PAD_SMALL );

    v2->setSpacing( HIG::PAD );

    QStringList headers;
    headers << tr("Trackers");
    myTrackerTree = new QTreeWidget;
    myTrackerTree->setHeaderLabels( headers );
    myTrackerTree->setSelectionMode( QTreeWidget::ExtendedSelection );
    myTrackerTree->setRootIsDecorated( false );
    myTrackerTree->setIndentation( 2 );
    myTrackerTree->setItemsExpandable( false );
    myTrackerTree->setTextElideMode( Qt::ElideRight );
    myTrackerTree->setAlternatingRowColors( true );
    connect( myTrackerTree, SIGNAL(itemSelectionChanged()), this, SLOT(onTrackerSelectionChanged()));
    h->addWidget( myTrackerTree, 1 );

    p = new QPushButton();
    p->setIcon( getStockIcon( "list-add", QStyle::SP_DialogOpenButton ) );
    p->setToolTip( "Add Tracker" );
    myAddTrackerButton = p;
    v2->addWidget( p, 1 );
    connect( p, SIGNAL(clicked(bool)), this, SLOT(onAddTrackerPushed()));

    p = new QPushButton();
    p->setIcon( getStockIcon( "document-properties", QStyle::SP_DesktopIcon ) );
    p->setToolTip( "Edit Tracker" );
    myAddTrackerButton = p;
    p->setEnabled( false );
    myEditTrackerButton = p;
    v2->addWidget( p, 1 );
    connect( p, SIGNAL(clicked(bool)), this, SLOT(onEditTrackerPushed()));

    p = new QPushButton();
    p->setIcon( getStockIcon( "list-remove", QStyle::SP_TrashIcon ) );
    p->setToolTip( "Remove Trackers" );
    p->setEnabled( false );
    myRemoveTrackerButton = p;
    v2->addWidget( p, 1 );
    connect( p, SIGNAL(clicked(bool)), this, SLOT(onRemoveTrackerPushed()));

    v2->addStretch( 1 );

    h->addLayout( v2, 1 );
    h->setStretch( 1, 0 );

    v->addLayout( h, 1 );

    c = new QCheckBox( tr( "Show &more details" ) );
    c->setChecked( myPrefs.getBool( Prefs::SHOW_TRACKER_SCRAPES ) );
    myShowTrackerScrapesCheck = c;
    v->addWidget( c, 1 );
    connect( c, SIGNAL(clicked(bool)), this, SLOT(onShowTrackerScrapesToggled(bool)) );

    return top;
}

/***
****
***/

QWidget *
Details :: createPeersTab( )
{
    QWidget * top = new QWidget;
    QVBoxLayout * v = new QVBoxLayout( top );
    v->setSpacing( HIG :: PAD_BIG );
    v->setContentsMargins( HIG::PAD_BIG, HIG::PAD_BIG, HIG::PAD_BIG, HIG::PAD_BIG );

    QStringList headers;
    headers << QString() << tr("Up") << tr("Down") << tr("%") << tr("Status") << tr("Address") << tr("Client");
    myPeerTree = new QTreeWidget;
    myPeerTree->setUniformRowHeights( true );
    myPeerTree->setHeaderLabels( headers );
    myPeerTree->setColumnWidth( 0, 20 );
    myPeerTree->setSortingEnabled( true );
    myPeerTree->sortByColumn( COL_ADDRESS, Qt::AscendingOrder );
    myPeerTree->setRootIsDecorated( false );
    myPeerTree->setTextElideMode( Qt::ElideRight );
    v->addWidget( myPeerTree, 1 );

    const QFontMetrics m( font( ) );
    QSize size = m.size( 0, "1024 MiB/s" );
    myPeerTree->setColumnWidth( COL_UP, size.width( ) );
    myPeerTree->setColumnWidth( COL_DOWN, size.width( ) );
    size = m.size( 0, " 100% " );
    myPeerTree->setColumnWidth( COL_PERCENT, size.width( ) );
    size = m.size( 0, "ODUK?EXI" );
    myPeerTree->setColumnWidth( COL_STATUS, size.width( ) );
    size = m.size( 0, "888.888.888.888" );
    myPeerTree->setColumnWidth( COL_ADDRESS, size.width( ) );
    size = m.size( 0, "Some BitTorrent Client" );
    myPeerTree->setColumnWidth( COL_CLIENT, size.width( ) );
    myPeerTree->setAlternatingRowColors( true );

    return top;
}

/***
****
***/

QWidget *
Details :: createFilesTab( )
{
    myFileTreeView = new FileTreeView( );

    connect( myFileTreeView, SIGNAL(      priorityChanged(const QSet<int>&, int)),
             this,           SLOT(  onFilePriorityChanged(const QSet<int>&, int)));

    connect( myFileTreeView, SIGNAL(      wantedChanged(const QSet<int>&, bool)),
             this,           SLOT(  onFileWantedChanged(const QSet<int>&, bool)));

    return myFileTreeView;
}

void
Details :: onFilePriorityChanged( const QSet<int>& indices, int priority )
{
    QString key;
    switch( priority ) {
        case TR_PRI_LOW:   key = "priority-low"; break;
        case TR_PRI_HIGH:  key = "priority-high"; break;
        default:           key = "priority-normal"; break;
    }
    mySession.torrentSet( myIds, key, indices.toList( ) );
}

void
Details :: onFileWantedChanged( const QSet<int>& indices, bool wanted )
{
    QString key( wanted ? "files-wanted" : "files-unwanted" );
    mySession.torrentSet( myIds, key, indices.toList( ) );
}
