// c/c++
#include <math.h>
#include <unistd.h>
#include <iostream>
#include <algorithm>
using namespace std;

//qt
#include <QApplication>
#include <QKeyEvent>
#include <QDateTime>

// myth
#include "mythcontext.h"
#include "mythdbcon.h"
#include "mythverbose.h"
#include "dbchannelinfo.h"
#include "programinfo.h"
#include "scheduledrecording.h"
#include "oldsettings.h"
#include "tv_play.h"
#include "tv_rec.h"
#include "proglist.h"
#include "customedit.h"
#include "util.h"
#include "remoteutil.h"
#include "channelutil.h"
#include "cardutil.h"
#include "mythuibuttonlist.h"
#include "mythuiguidegrid.h"
#include "mythdialogbox.h"
#include "progfind.h"

#include "guidegrid.h"

QWaitCondition epgIsVisibleCond;

#define LOC      QString("GuideGrid: ")
#define LOC_ERR  QString("GuideGrid, Error: ")
#define LOC_WARN QString("GuideGrid, Warning: ")

JumpToChannel::JumpToChannel(
    JumpToChannelListener *parent, const QString &start_entry,
    int start_chan_idx, int cur_chan_idx, uint rows_disp) :
    listener(parent),
    entry(start_entry),
    previous_start_channel_index(start_chan_idx),
    previous_current_channel_index(cur_chan_idx),
    rows_displayed(rows_disp),
    timer(new QTimer(this))
{
    if (parent && timer)
    {
        connect(timer, SIGNAL(timeout()), SLOT(deleteLater()));
        timer->setSingleShot(true);
    }
    Update();
}


void JumpToChannel::deleteLater(void)
{
    if (listener)
    {
        listener->SetJumpToChannel(NULL);
        listener = NULL;
    }

    if (timer)
    {
        timer->stop();
        timer = NULL;
    }

    QObject::deleteLater();
}


static bool has_action(QString action, const QStringList &actions)
{
    QStringList::const_iterator it;
    for (it = actions.begin(); it != actions.end(); ++it)
    {
        if (action == *it)
            return true;
    }
    return false;
}

bool JumpToChannel::ProcessEntry(
    const QStringList &actions, const QKeyEvent *e)
{
    if (!listener)
        return false;

    if (has_action("ESCAPE", actions))
    {
        listener->GoTo(previous_start_channel_index,
                       previous_current_channel_index);
        deleteLater();
        return true;
    }

    if (has_action("DELETE", actions))
    {
        if (entry.length())
            entry = entry.left(entry.length()-1);
        Update();
        return true;
    }

    if (has_action("SELECT", actions))
    {
        if (Update())
            deleteLater();
        return true;
    }

    QString txt = e->text();
    bool isUInt;
    txt.toUInt(&isUInt);
    if (isUInt)
    {
        entry += txt;
        Update();
        return true;
    }

    if (entry.length() && (txt=="_" || txt=="-" || txt=="#" || txt=="."))
    {
        entry += txt;
        Update();
        return true;
    }

    return false;
}

bool JumpToChannel::Update(void)
{
    if (!timer || !listener)
        return false;

    timer->stop();

    // find the closest channel ...
    int i = listener->FindChannel(0, entry, false);
    if (i >= 0)
    {
        // setup the timeout timer for jump mode
        timer->start(kJumpToChannelTimeout);

        // rows_displayed to center
        int start = i - rows_displayed/2;
        int cur   = rows_displayed/2;
        listener->GoTo(start, cur);
        return true;
    }
    else
    { // prefix must be invalid.. reset entry..
        deleteLater();
        return false;
    }
}

void GuideGrid::RunProgramGuide(uint chanid, const QString &channum,
                    TV *player, bool embedVideo, bool allowFinder, int changrpid)
{
    MythScreenStack *mainStack = GetMythMainWindow()->GetMainStack();
    GuideGrid *gg = new GuideGrid(mainStack,
                                  chanid, channum,
                                  player, embedVideo, allowFinder,
                                  changrpid);

    if (gg->Create())
        mainStack->AddScreen(gg, (player == NULL));
    else
        delete gg;
}

GuideGrid::GuideGrid(MythScreenStack *parent,
                     uint chanid, QString channum,
                     TV *player, bool embedVideo,
                     bool allowFinder, int changrpid)
         : MythScreenType(parent, "guidegrid"),
    m_allowFinder(allowFinder),
    m_player(player),
    m_usingNullVideo(false), m_embedVideo(embedVideo),
    previewVideoRefreshTimer(new QTimer(this)),
    m_jumpToChannelLock(QMutex::Recursive),
    m_jumpToChannel(NULL),
    m_jumpToChannelEnabled(true)
{
    connect(previewVideoRefreshTimer, SIGNAL(timeout()),
            this,                     SLOT(refreshVideo()));

    m_channelCount = 5;
    m_timeCount = 30;
    m_currentStartChannel = 0;
    m_changrpid = changrpid;
    m_changrplist = ChannelGroup::GetChannelGroups();

    m_jumpToChannelEnabled = gContext->GetNumSetting("EPGEnableJumpToChannel", 1);
    m_sortReverse = gContext->GetNumSetting("EPGSortReverse", 0);
    m_selectChangesChannel = gContext->GetNumSetting("SelectChangesChannel", 0);
    m_selectRecThreshold = gContext->GetNumSetting("SelChangeRecThreshold", 16);

    m_timeFormat = gContext->GetSetting("TimeFormat", "h:mm AP");
    m_dateFormat = gContext->GetSetting("ShortDateFormat", "ddd d");

    m_channelOrdering = gContext->GetSetting("ChannelOrdering", "channum");
    m_channelFormat = gContext->GetSetting("ChannelFormat", "<num> <sign>");
    m_channelFormat.replace(" ", "\n");

    m_unknownTitle = gContext->GetSetting("UnknownTitle", "Unknown");
    m_unknownCategory = gContext->GetSetting("UnknownCategory", "Unknown");

    for (int y = 0; y < MAX_DISPLAY_CHANS; y++)
        m_programs[y] = NULL;

    for (int x = 0; x < MAX_DISPLAY_TIMES; x++)
    {
        for (int y = 0; y < MAX_DISPLAY_CHANS; y++)
            m_programInfos[y][x] = NULL;
    }

    m_originalStartTime = QDateTime::currentDateTime();

    int secsoffset = -((m_originalStartTime.time().minute() % 30) * 60 +
                        m_originalStartTime.time().second());
    m_currentStartTime = m_originalStartTime.addSecs(secsoffset);
    m_startChanID  = chanid;
    m_startChanNum = channum;

    if (m_player)
        m_embedVideo = m_player->IsRunning() && m_embedVideo;
}

bool GuideGrid::Create()
{
    QString windowName = "programguide";

    if (m_embedVideo)
        windowName = "programguide-video";

    if (!LoadWindowFromXML("schedule-ui.xml", windowName, this))
        return false;

    bool err = false;
    UIUtilE::Assign(this, m_timeList, "timelist", &err);
    UIUtilE::Assign(this, m_channelList, "channellist", &err);
    UIUtilE::Assign(this, m_guideGrid, "guidegrid", &err);
    UIUtilW::Assign(this, m_dateText, "datetext");
    UIUtilW::Assign(this, m_changroupname, "channelgroup");
    UIUtilW::Assign(this, m_channelImage, "channelicon");
    UIUtilW::Assign(this, m_jumpToText, "jumptotext");

    if (err)
    {
        VERBOSE(VB_IMPORTANT, QString("Cannot load screen '%1'").arg(windowName));
        return false;
    }

    BuildFocusList();

    MythUIImage *videoImage = dynamic_cast<MythUIImage *>(GetChild("video"));
    if (videoImage && m_embedVideo)
        m_videoRect = videoImage->GetArea();
    else
        m_videoRect = QRect(0,0,1,1);

    m_channelCount = m_guideGrid->getChannelCount();
    m_timeCount = m_guideGrid->getTimeCount() * 6;
    m_verticalLayout = m_guideGrid->isVerticalLayout();

    m_currentRow = (int)(m_channelCount / 2);
    m_currentCol = 0;

    fillTimeInfos();
    fillChannelInfos();
    int maxchannel = max((int)GetChannelCount() - 1, 0);
    setStartChannel((int)(m_currentStartChannel) - (int)(m_channelCount / 2));
    m_channelCount = min(m_channelCount, maxchannel + 1);
    updateChannels();

    m_recList.FromScheduler();
    fillProgramInfos();
    updateInfo();

    m_updateTimer = NULL;
    m_updateTimer = new QTimer(this);
    connect(m_updateTimer, SIGNAL(timeout()), SLOT(updateTimeout()) );
    m_updateTimer->start(60 * 1000);

    if (m_dateText)
        m_dateText->SetText(m_currentStartTime.toString(m_dateFormat));

    QString changrpname = ChannelGroup::GetChannelGroupName(m_changrpid);

    if (m_changroupname)
        m_changroupname->SetText(changrpname);

    gContext->addListener(this);

    return true;
}

GuideGrid::~GuideGrid()
{
    gContext->removeListener(this);

    for (int y = 0; y < MAX_DISPLAY_CHANS; y++)
    {
        if (m_programs[y])
        {
            delete m_programs[y];
            m_programs[y] = NULL;
        }
    }

    m_channelInfos.clear();

    if (m_updateTimer)
    {
        m_updateTimer->disconnect(this);
        m_updateTimer->deleteLater();
    }

    if (previewVideoRefreshTimer)
    {
        previewVideoRefreshTimer->disconnect(this);
        previewVideoRefreshTimer = NULL;
    }

    gContext->SaveSetting("EPGSortReverse", m_sortReverse ? "1" : "0");

    // if we have a player and we are returning to it we need
    // to tell it to stop embedding and return to fullscreen
    if (m_player && m_allowFinder)
    {
        QString message = QString("EPG_EXITING");
        qApp->postEvent(m_player, new MythEvent(message));
    }

    // maybe the user selected a different channel group,
    // tell the player to update its channel list just in case
    if (m_player)
        m_player->UpdateChannelList(m_changrpid);
}

bool GuideGrid::keyPressEvent(QKeyEvent *event)
{
    QStringList actions;
    bool handled = false;
    handled = GetMythMainWindow()->TranslateKeyPress("TV Frontend", event, actions);

    if (handled)
        return true;

    // We want to handle jump to channel before everything else
    // The reason is because the number keys could be mapped to
    // other things. If this is the case, then the jump to channel
    // will not work correctly.
    {
        QMutexLocker locker(&m_jumpToChannelLock);

        if (!m_jumpToChannel || m_jumpToChannelEnabled)
        {
            bool isNum;
            event->text().toInt(&isNum);
            if (isNum && !m_jumpToChannel)
            {
                m_jumpToChannel = new JumpToChannel(
                    this, event->text(),
                    m_currentStartChannel, m_currentRow, m_channelCount);
                updateJumpToChannel();
                handled = true;
            }
        }

        if (m_jumpToChannel && !handled)
            handled = m_jumpToChannel->ProcessEntry(actions, event);
    }

    for (int i = 0; i < actions.size() && !handled; i++)
    {
        QString action = actions[i];
        handled = true;
        if (action == "UP")
        {
            if (m_verticalLayout)
                cursorLeft();
            else
                cursorUp();
        }
        else if (action == "DOWN")
        {
            if (m_verticalLayout)
                cursorRight();
            else
                cursorDown();
        }
        else if (action == "LEFT")
        {
            if (m_verticalLayout)
                cursorUp();
            else
                cursorLeft();
        }
        else if (action == "RIGHT")
        {
            if (m_verticalLayout)
                cursorDown();
            else
                cursorRight();
        }
        else if (action == "PAGEUP")
        {
            if (m_verticalLayout)
                pageLeft();
            else
                pageUp();
        }
        else if (action == "PAGEDOWN")
        {
            if (m_verticalLayout)
                pageRight();
            else
                pageDown();
        }
        else if (action == "PAGELEFT")
        {
            if (m_verticalLayout)
                pageUp();
            else
                pageLeft();
        }
        else if (action == "PAGERIGHT")
        {
            if (m_verticalLayout)
                pageDown();
            else
                pageRight();
        }
        else if (action == "DAYLEFT")
            dayLeft();
        else if (action == "DAYRIGHT")
            dayRight();
        else if (action == "NEXTFAV")
            toggleGuideListing();
        else if (action == "FINDER")
            showProgFinder();
        else if (action == "MENU")
            showMenu();
        else if (action == "ESCAPE" || action == "GUIDE")
            escape();
        else if (action == "SELECT")
        {
            if (m_player && m_selectChangesChannel)
            {
                // See if this show is far enough into the future that it's probable
                // that the user wanted to schedule it to record instead of changing the channel.
                ProgramInfo *pginfo = m_programInfos[m_currentRow][m_currentCol];
                if (pginfo && (pginfo->title != m_unknownTitle) &&
                    ((pginfo->SecsTillStart() / 60) >= m_selectRecThreshold))
                {
                    editRecording();
                }
                else
                {
                    enter();
                }
            }
            else
                editRecording();
        }
        else if (action == "INFO")
            editScheduled();
        else if (action == "CUSTOMEDIT")
            customEdit();
        else if (action == "DELETE")
            deleteRule();
        else if (action == "UPCOMING")
            upcoming();
        else if (action == "DETAILS")
            details();
        else if (action == "TOGGLERECORD")
            quickRecord();
        else if (action == "TOGGLEFAV")
        {
            if (m_changrpid == -1)
                ChannelGroupMenu(0);
            else
                toggleChannelFavorite();
        }
        else if (action == "CHANUPDATE")
            channelUpdate();
        else if (action == "VOLUMEUP")
            volumeUpdate(true);
        else if (action == "VOLUMEDOWN")
            volumeUpdate(false);
        else if (action == "MUTE")
            toggleMute();
        else if (action == "TOGGLEEPGORDER")
        {
            m_sortReverse = !m_sortReverse;
            generateListings();
            updateChannels();
        }
        else
            handled = false;
    }

    if (!handled && MythScreenType::keyPressEvent(event))
        handled = true;

    return handled;
}

void GuideGrid::showMenu(void)
{
    QString label = tr("Options");

    MythScreenStack *popupStack = GetMythMainWindow()->GetStack("popup stack");
    MythDialogBox *menuPopup = new MythDialogBox(label, popupStack, "menuPopup");

    if (menuPopup->Create())
    {
        menuPopup->SetReturnEvent(this, "menu");

        menuPopup->AddButton(tr("Record"));
        menuPopup->AddButton(tr("Edit Schedule"));
        menuPopup->AddButton(tr("Program Details"));
        menuPopup->AddButton(tr("Upcoming"));
        menuPopup->AddButton(tr("Custom Edit"));

        ProgramInfo *pginfo = m_programInfos[m_currentRow][m_currentCol];
        if (pginfo && pginfo->recordid > 0)
            menuPopup->AddButton(tr("Delete Rule"));

        menuPopup->AddButton(tr("Reverse Channel Order"));

        if (m_changrpid == -1)
            menuPopup->AddButton(tr("Add To Channel Group"));
        else
            menuPopup->AddButton(tr("Remove from Channel Group"));

        menuPopup->AddButton(tr("Choose Channel Group"));

        menuPopup->AddButton(tr("Cancel"));

        popupStack->AddScreen(menuPopup);
    }
    else
    {
        delete menuPopup;
    }
}

PixmapChannel *GuideGrid::GetChannelInfo(uint chan_idx, int sel)
{
    sel = (sel >= 0) ? sel : m_channelInfoIdx[chan_idx];

    if (chan_idx >= GetChannelCount())
        return NULL;

    if (sel >= (int) m_channelInfos[chan_idx].size())
        return NULL;

    return &(m_channelInfos[chan_idx][sel]);
}

const PixmapChannel *GuideGrid::GetChannelInfo(uint chan_idx, int sel) const
{
    return ((GuideGrid*)this)->GetChannelInfo(chan_idx, sel);
}

uint GuideGrid::GetChannelCount(void) const
{
    return m_channelInfos.size();
}

int GuideGrid::GetStartChannelOffset(int row) const
{
    uint cnt = GetChannelCount();
    if (!cnt)
        return -1;

    row = (row < 0) ? m_currentRow : row;
    return (row + m_currentStartChannel) % cnt;
}

ProgramList GuideGrid::GetProgramList(uint chanid) const
{
    ProgramList proglist;
    MSqlBindings bindings;
    QString querystr =
        "WHERE program.chanid     = :CHANID  AND "
        "      program.endtime   >= :STARTTS AND "
        "      program.starttime <= :ENDTS   AND "
        "      program.manualid   = 0 ";
    bindings[":STARTTS"] = m_currentStartTime.toString("yyyy-MM-ddThh:mm:00");
    bindings[":ENDTS"]   = m_currentEndTime.toString("yyyy-MM-ddThh:mm:00");
    bindings[":CHANID"]  = chanid;

    ProgramList dummy;
    proglist.FromProgram(querystr, bindings, dummy);

    return proglist;
}

uint GuideGrid::GetAlternateChannelIndex(
    uint chan_idx, bool with_same_channum) const
{
    uint si = m_channelInfoIdx[chan_idx];
    const PixmapChannel *chinfo = GetChannelInfo(chan_idx, si);

    PlayerContext *ctx = m_player->GetPlayerReadLock(-1, __FILE__, __LINE__);

    const uint cnt = (ctx && chinfo) ? m_channelInfos[chan_idx].size() : 0;
    for (uint i = 0; i < cnt; ++i)
    {
        if (i == si)
            continue;

        const PixmapChannel *ciinfo = GetChannelInfo(chan_idx, i);
        if (!ciinfo)
            continue;

        bool same_channum = ciinfo->channum == chinfo->channum;

        if (with_same_channum != same_channum)
            continue;

        if (!m_player->IsTunable(ctx, ciinfo->chanid, true))
            continue;

        if (with_same_channum ||
            (GetProgramList(chinfo->chanid) ==
             GetProgramList(ciinfo->chanid)))
        {
            si = i;
            break;
        }
    }

    m_player->ReturnPlayerLock(ctx);

    return si;
}


#define MKKEY(IDX,SEL) ((((uint64_t)IDX) << 32) | SEL)
DBChanList GuideGrid::GetSelection(void) const
{
    DBChanList selected;

    int idx = GetStartChannelOffset();
    if (idx < 0)
        return selected;

    uint si  = m_channelInfoIdx[idx];

    vector<uint64_t> sel;
    sel.push_back( MKKEY(idx, si) );

    const PixmapChannel *ch = GetChannelInfo(sel[0]>>32, sel[0]&0xffff);
    if (!ch)
        return selected;

    selected.push_back(*ch);
    if (m_channelInfos[idx].size() <= 1)
        return selected;

    ProgramList proglist = GetProgramList(selected[0].chanid);

    if (proglist.empty())
        return selected;

    for (uint i = 0; i < m_channelInfos[idx].size(); i++)
    {
        const PixmapChannel *ci = GetChannelInfo(idx, i);
        if (ci && (i != si) &&
            (ci->callsign == ch->callsign) && (ci->channum  == ch->channum))
        {
            sel.push_back( MKKEY(idx, i) );
        }
    }

    for (uint i = 0; i < m_channelInfos[idx].size(); i++)
    {
        const PixmapChannel *ci = GetChannelInfo(idx, i);
        if (ci && (i != si) &&
            (ci->callsign == ch->callsign) && (ci->channum  != ch->channum))
        {
            sel.push_back( MKKEY(idx, i) );
        }
    }

    for (uint i = 0; i < m_channelInfos[idx].size(); i++)
    {
        const PixmapChannel *ci = GetChannelInfo(idx, i);
        if ((i != si) && (ci->callsign != ch->callsign))
        {
            sel.push_back( MKKEY(idx, i) );
        }
    }

    for (uint i = 1; i < sel.size(); i++)
    {
        const PixmapChannel *ci = GetChannelInfo(sel[i]>>32, sel[i]&0xffff);
        if (!ci)
            continue;

        ProgramList ch_proglist = GetProgramList(ch->chanid);
        if (proglist == ch_proglist)
            selected.push_back(*ci);
    }

    return selected;
}
#undef MKKEY

void GuideGrid::updateTimeout(void)
{
    m_updateTimer->stop();
    fillProgramInfos();
    m_updateTimer->start((int)(60 * 1000));
}

void GuideGrid::fillChannelInfos(bool gotostartchannel)
{
    m_channelInfos.clear();
    m_channelInfoIdx.clear();
    m_currentStartChannel = 0;

    DBChanList channels = ChannelUtil::GetChannels(0, true, "", m_changrpid);
    ChannelUtil::SortChannels(channels, m_channelOrdering, false);

    typedef vector<uint> uint_list_t;
    QMap<QString,uint_list_t> channum_to_index_map;
    QMap<QString,uint_list_t> callsign_to_index_map;

    for (uint i = 0; i < channels.size(); i++)
    {
        uint chan = i;
        if (m_sortReverse)
        {
            chan = channels.size() - i - 1;
        }

        bool ndup = channum_to_index_map[channels[chan].channum].size();
        bool cdup = callsign_to_index_map[channels[chan].callsign].size();

        if (ndup && cdup)
            continue;

        PixmapChannel val(channels[chan]);

        channum_to_index_map[val.channum].push_back(GetChannelCount());
        callsign_to_index_map[val.callsign].push_back(GetChannelCount());

        // add the new channel to the list
        pix_chan_list_t tmp;
        tmp.push_back(val);
        m_channelInfos.push_back(tmp);
    }

    // handle duplicates
    for (uint i = 0; i < channels.size(); i++)
    {
        const uint_list_t &ndups = channum_to_index_map[channels[i].channum];
        for (uint j = 0; j < ndups.size(); j++)
        {
            if (channels[i].chanid != m_channelInfos[ndups[j]][0].chanid)
                m_channelInfos[ndups[j]].push_back(channels[i]);
        }

        const uint_list_t &cdups = callsign_to_index_map[channels[i].callsign];
        for (uint j = 0; j < cdups.size(); j++)
        {
            if (channels[i].chanid != m_channelInfos[cdups[j]][0].chanid)
                m_channelInfos[cdups[j]].push_back(channels[i]);
        }
    }

    if (gotostartchannel)
    {
        int ch = FindChannel(m_startChanID, m_startChanNum);
        m_currentStartChannel = (uint) max(0, ch);
    }

    if (m_channelInfos.empty())
    {
        VERBOSE(VB_IMPORTANT, "GuideGrid: "
                "\n\t\t\tYou don't have any channels defined in the database."
                "\n\t\t\tGuide grid will have nothing to show you.");
    }
}

int GuideGrid::FindChannel(uint chanid, const QString &channum,
                           bool exact) const
{
    static QMutex chanSepRegExpLock;
    static QRegExp chanSepRegExp(ChannelUtil::kATSCSeparators);

    // first check chanid
    uint i = (chanid) ? 0 : GetChannelCount();
    for (; i < GetChannelCount(); i++)
    {
        if (m_channelInfos[i][0].chanid == chanid)
                return i;
    }

    // then check for chanid in duplicates
    i = (chanid) ? 0 : GetChannelCount();
    for (; i < GetChannelCount(); i++)
    {
        for (uint j = 1; j < m_channelInfos[i].size(); j++)
        {
            if (m_channelInfos[i][j].chanid == chanid)
                return i;
        }
    }

    // then check channum, first only
    i = (channum.isEmpty()) ? GetChannelCount() : 0;
    for (; i < GetChannelCount(); i++)
    {
         if (m_channelInfos[i][0].channum == channum)
            return i;
    }

    // then check channum duplicates
    i = (channum.isEmpty()) ? GetChannelCount() : 0;
    for (; i < GetChannelCount(); i++)
    {
        for (uint j = 1; j < m_channelInfos[i].size(); j++)
        {
            if (m_channelInfos[i][j].channum == channum)
                return i;
        }
    }

    if (exact || channum.isEmpty())
        return -1;

    // then check partial channum, first only
    for (i = 0; i < GetChannelCount(); i++)
    {
        if (m_channelInfos[i][0].channum.left(channum.length()) == channum)
            return i;
    }

    // then check all partial channum
    for (i = 0; i < GetChannelCount(); i++)
    {
        for (uint j = 0; j < m_channelInfos[i].size(); j++)
        {
            if (m_channelInfos[i][j].channum.left(channum.length()) == channum)
                return i;
        }
    }

    // then check all channum with "_" for subchannels
    QMutexLocker locker(&chanSepRegExpLock);
    QString tmpchannum = channum;
    if (tmpchannum.contains(chanSepRegExp))
    {
        tmpchannum.replace(chanSepRegExp, "_");
    }
    else if (channum.length() >= 2)
    {
        tmpchannum = channum.left(channum.length() - 1) + "_" +
            channum.right(1);
    }
    else
    {
        return -1;
    }

    for (i = 0; i < GetChannelCount(); i++)
    {
        for (uint j = 0; j < m_channelInfos[i].size(); j++)
        {
            QString tmp = m_channelInfos[i][j].channum;
            tmp.replace(chanSepRegExp, "_");
            if (tmp == tmpchannum)
                return i;
        }
    }

    return -1;
}

void GuideGrid::fillTimeInfos()
{
    m_timeList->Reset();

    QDateTime t = m_currentStartTime;
    int cnt = 0;

    m_firstTime = m_currentStartTime;
    m_lastTime = m_firstTime.addSecs(m_timeCount * 60 * 4);

    for (int x = 0; x < m_timeCount; x++)
    {
        int mins = t.time().minute();
        mins = 5 * (mins / 5);
        if (mins % 30 == 0)
        {
            int hour = t.time().hour();
            QString timeStr = QTime(hour, mins).toString(m_timeFormat);
            new MythUIButtonListItem(m_timeList, timeStr);

            cnt++;
        }

        t = t.addSecs(5 * 60);
    }
    m_currentEndTime = t;
}

void GuideGrid::fillProgramInfos(void)
{
    m_guideGrid->ResetData();

    for (int y = 0; y < m_channelCount; y++)
    {
        fillProgramRowInfos(y);
    }
}

void GuideGrid::fillProgramRowInfos(unsigned int row)
{
    m_guideGrid->ResetRow(row);

    ProgramList *proglist;

    if (m_programs[row])
        delete m_programs[row];
    m_programs[row] = NULL;

    for (int x = 0; x < m_timeCount; x++)
    {
        m_programInfos[row][x] = NULL;
    }

    if (m_channelInfos.size() == 0)
        return;

    int chanNum = row + m_currentStartChannel;
    if (chanNum >= (int) m_channelInfos.size())
        chanNum -= (int) m_channelInfos.size();
    if (chanNum >= (int) m_channelInfos.size())
        return;

    if (chanNum < 0)
        chanNum = 0;

    m_programs[row] = proglist = new ProgramList();

    MSqlBindings bindings;
    QString querystr = "WHERE program.chanid = :CHANID "
                       "  AND program.endtime >= :STARTTS "
                       "  AND program.starttime <= :ENDTS "
                       "  AND program.manualid = 0 ";
    bindings[":CHANID"]  = GetChannelInfo(chanNum)->chanid;
    bindings[":STARTTS"] = m_currentStartTime.toString("yyyy-MM-ddThh:mm:00");
    bindings[":ENDTS"] = m_currentEndTime.toString("yyyy-MM-ddThh:mm:00");

    proglist->FromProgram(querystr, bindings, m_recList);

    QDateTime ts = m_currentStartTime;

    QDateTime tnow = QDateTime::currentDateTime();
    int progPast = 0;
    if (tnow > m_currentEndTime)
        progPast = 100;
    else if (tnow < m_currentStartTime)
        progPast = 0;
    else
    {
        int played = m_currentStartTime.secsTo(tnow);
        int length = m_currentStartTime.secsTo(m_currentEndTime);
        if (length)
            progPast = played * 100 / length;
    }

    m_guideGrid->SetProgPast(progPast);

    ProgramList::iterator program;
    program = proglist->begin();
    vector<ProgramInfo*> unknownlist;
    bool unknown = false;
    ProgramInfo *proginfo = NULL;
    for (int x = 0; x < m_timeCount; x++)
    {
        if (program != proglist->end() && (ts >= (*program)->endts))
        {
            ++program;
        }

        if ((program == proglist->end()) || (ts < (*program)->startts))
        {
            if (unknown)
            {
                proginfo->spread++;
                proginfo->endts = proginfo->endts.addSecs(5 * 60);
            }
            else
            {
                proginfo = new ProgramInfo;
                unknownlist.push_back(proginfo);
                proginfo->title = m_unknownTitle;
                proginfo->category = m_unknownCategory;
                proginfo->startCol = x;
                proginfo->spread = 1;
                proginfo->startts = ts;
                proginfo->endts = proginfo->startts.addSecs(5 * 60);
                unknown = true;
            }
        }
        else
        {
            if (proginfo == *program)
            {
                proginfo->spread++;
            }
            else
            {
                proginfo = *program;
                proginfo->startCol = x;
                proginfo->spread = 1;
                unknown = false;
            }
        }
        m_programInfos[row][x] = proginfo;
        ts = ts.addSecs(5 * 60);
    }

    vector<ProgramInfo*>::iterator it = unknownlist.begin();
    for (; it != unknownlist.end(); ++it)
        proglist->append(*it);

    MythRect programRect = m_guideGrid->GetArea();

    int ydifference = 0;
    int xdifference = 0;

    if (m_verticalLayout)
    {
        ydifference = programRect.width() / m_guideGrid->getChannelCount();
        xdifference = programRect.height() / m_timeCount;
    }
    else
    {
        ydifference = programRect.height() / m_guideGrid->getChannelCount();
        xdifference = programRect.width() / m_timeCount;
    }

    int arrow = 0;
    int cnt = 0;
    int spread = 1;
    QDateTime lastprog;
    QRect tempRect;
    bool isCurrent = false;

    for (int x = 0; x < m_timeCount; x++)
    {
        ProgramInfo *pginfo = m_programInfos[row][x];
        if (!pginfo)
            continue;

        spread = 1;
        if (pginfo->startts != lastprog)
        {
            arrow = 0;
            if (pginfo->startts < m_firstTime.addSecs(-300))
                arrow = arrow + 1;
            if (pginfo->endts > m_lastTime.addSecs(2100))
                arrow = arrow + 2;

            if (pginfo->spread != -1)
            {
                spread = pginfo->spread;
            }
            else
            {
                for (int z = x + 1; z < m_timeCount; z++)
                {
                    ProgramInfo *test = m_programInfos[row][z];
                    if (test && test->startts == pginfo->startts)
                        spread++;
                }
                pginfo->spread = spread;
                pginfo->startCol = x;

                for (int z = x + 1; z < x + spread; z++)
                {
                    ProgramInfo *test = m_programInfos[row][z];
                    if (test)
                    {
                        test->spread = spread;
                        test->startCol = x;
                    }
                }
            }

            if (m_verticalLayout)
            {
                tempRect = QRect((int)(row * ydifference), (int)(x * xdifference),
                            (int)(ydifference), xdifference * pginfo->spread);
            }
            else
            {
                tempRect = QRect((int)(x * xdifference), (int)(row * ydifference),
                            (int)(xdifference * pginfo->spread), ydifference);
            }

            if (m_currentRow == (int)row && (m_currentCol >= x) &&
                (m_currentCol < (x + spread)))
                isCurrent = true;
            else
                isCurrent = false;

            int recFlag;
            switch (pginfo->rectype)
            {
            case kSingleRecord:
                recFlag = 1;
                break;
            case kTimeslotRecord:
                recFlag = 2;
                break;
            case kChannelRecord:
                recFlag = 3;
                break;
            case kAllRecord:
                recFlag = 4;
                break;
            case kWeekslotRecord:
                recFlag = 5;
                break;
            case kFindOneRecord:
            case kFindDailyRecord:
            case kFindWeeklyRecord:
                recFlag = 6;
                break;
            case kOverrideRecord:
            case kDontRecord:
                recFlag = 7;
                break;
            case kNotRecording:
            default:
                recFlag = 0;
                break;
            }

            int recStat;
            if (pginfo->recstatus == rsConflict ||
                pginfo->recstatus == rsOffLine)
                recStat = 2;
            if (pginfo->recstatus <= rsWillRecord)
                recStat = 1;
            else
                recStat = 0;

            m_guideGrid->SetProgramInfo(row, cnt, tempRect, pginfo->title,
                                    pginfo->category, arrow, recFlag,
                                    recStat, isCurrent);
            cnt++;
        }

        lastprog = pginfo->startts;
    }
}

void GuideGrid::customEvent(QEvent *event)
{
    if ((MythEvent::Type)(event->type()) == MythEvent::MythEventMessage)
    {
        MythEvent *me = (MythEvent *)event;
        QString message = me->Message();

        if (message == "SCHEDULE_CHANGE")
        {
            m_recList.FromScheduler();
            fillProgramInfos();
            updateInfo();
        }
    }
    else if (event->type() == kMythDialogBoxCompletionEventType)
    {
        DialogCompletionEvent *dce =
        dynamic_cast<DialogCompletionEvent*>(event);

        QString resultid= dce->GetId();
        QString resulttext  = dce->GetResultText();
        int buttonnum  = dce->GetResult();

        if (resultid == "deleterule")
        {
            ScheduledRecording *record = qVariantValue<ScheduledRecording *>(dce->GetData());
            if (record)
            {
                if (buttonnum > 0)
                {
                    record->remove();
                    ScheduledRecording::signalChange(record->getRecordID());
                }
                record->deleteLater();
            }
            EmbedTVWindow();
        }
        else if (resultid == "menu")
        {
            if (resulttext == tr("Record"))
            {
                quickRecord();
            }
            else if (resulttext == tr("Edit Schedule"))
            {
                editScheduled();
            }
            else if (resulttext == tr("Program Details"))
            {
                details();
            }
            else if (resulttext == tr("Upcoming"))
            {
                upcoming();
            }
            else if (resulttext == tr("Custom Edit"))
            {
                customEdit();
            }
            else if (resulttext == tr("Delete Rule"))
            {
                deleteRule();
            }
            else if (resulttext == tr("Reverse Channel Order"))
            {
                m_sortReverse = !m_sortReverse;
                generateListings();
                updateChannels();
            }
            else if (resulttext == tr("Add To Channel Group"))
            {
                if (m_changrpid == -1)
                    ChannelGroupMenu(0);
            }
            else if (resulttext == tr("Remove from Channel Group"))
            {
                toggleChannelFavorite();
            }
            else if (resulttext == tr("Choose Channel Group"))
            {
                ChannelGroupMenu(1);
            }
        }
    else if (resultid == "channelgrouptogglemenu")
        {
            if (resulttext != tr("Cancel"))
            {
                int changroupid;
                changroupid = ChannelGroup::GetChannelGroupId(resulttext);

                if (changroupid > 0)
                    toggleChannelFavorite(changroupid);
            }
        }
    else if (resultid == "channelgroupmenu")
        {
            if (resulttext != tr("Cancel"))
            {
                int changroupid;

                if (resulttext == QObject::tr("All Channels"))
                    changroupid = -1;
                else
                    changroupid = ChannelGroup::GetChannelGroupId(resulttext);

                m_changrpid = changroupid;
                generateListings();
                updateChannels();
                updateInfo();

                QString changrpname;
                changrpname = ChannelGroup::GetChannelGroupName(m_changrpid);

                if (m_changroupname)
                    m_changroupname->SetText(changrpname);
                else
                {
                    if (m_dateText)
                    {
                        m_dateText->SetText(changrpname);
                        QTimer::singleShot(5000, this, SLOT(infoTimeout()));
                    }
                }
            }
        }
    }
}

void GuideGrid::infoTimeout(void)
{
    if (m_dateText)
        m_dateText->SetText(m_currentStartTime.toString(m_dateFormat));
}

void GuideGrid::updateChannels(void)
{
    bool channelsChanged = false;

    m_channelList->Reset();

    PixmapChannel *chinfo = GetChannelInfo(m_currentStartChannel);

    if (m_player)
        m_player->ClearTunableCache();

    bool showChannelIcon = gContext->GetNumSetting("EPGShowChannelIcon", 0);

    for (unsigned int y = 0; (y < (unsigned int)m_channelCount) && chinfo; y++)
    {
        unsigned int chanNumber = y + m_currentStartChannel;
        if (chanNumber >= m_channelInfos.size())
            chanNumber -= m_channelInfos.size();
        if (chanNumber >= m_channelInfos.size())
            break;

        chinfo = GetChannelInfo(chanNumber);

        bool unavailable = false, try_alt = false;

        if (m_player)
        {
            const PlayerContext *ctx = m_player->GetPlayerReadLock(
                -1, __FILE__, __LINE__);
            if (ctx && chinfo)
                try_alt = !m_player->IsTunable(ctx, chinfo->chanid, true);
            m_player->ReturnPlayerLock(ctx);
        }

        if (try_alt)
        {
            unavailable = true;

            // Try alternates with same channum if applicable
            uint alt = GetAlternateChannelIndex(chanNumber, true);
            if (alt != m_channelInfoIdx[chanNumber])
            {
                unavailable = false;
                m_channelInfoIdx[chanNumber] = alt;
                chinfo = GetChannelInfo(chanNumber);
                channelsChanged = true;
            }

            // Try alternates with different channum if applicable
            if (unavailable && !GetProgramList(chinfo->chanid).empty())
            {
                alt = GetAlternateChannelIndex(chanNumber, false);
                unavailable = (alt == m_channelInfoIdx[chanNumber]);
            }
        }

        MythUIButtonListItem *item = new MythUIButtonListItem(
                m_channelList, chinfo->GetFormatted(m_channelFormat));

        QString state;
        if (unavailable)
            state = (m_changrpid == -1) ? "unavailable" : "favunavailable";
        else
            state = (m_changrpid == -1) ? "" : "favourite";

        item->SetText(chinfo->GetFormatted(m_channelFormat), "buttontext", state);

        if (showChannelIcon && !chinfo->icon.isEmpty())
        {
            if (chinfo->CacheChannelIcon())
            {
                QString localpath = chinfo->m_localIcon;
                item->SetImage(localpath, "channelicon");
            }
        }
    }
}

void GuideGrid::updateInfo(void)
{
    if (m_currentRow < 0 || m_currentCol < 0)
        return;

    ProgramInfo *pginfo = m_programInfos[m_currentRow][m_currentCol];
    if (!pginfo)
        return;

    InfoMap infoMap;

    int chanNum = m_currentRow + m_currentStartChannel;
    if (chanNum >= (int)m_channelInfos.size())
        chanNum -= (int)m_channelInfos.size();
    if (chanNum >= (int)m_channelInfos.size())
        return;
    if (chanNum < 0)
        chanNum = 0;

    PixmapChannel *chinfo = GetChannelInfo(chanNum);

    bool showChannelIcon = gContext->GetNumSetting("EPGShowChannelIcon", 0);

    if (m_channelImage)
    {
        m_channelImage->Reset();
        if (showChannelIcon && !chinfo->icon.isEmpty())
        {
            if (chinfo->CacheChannelIcon())
            {
                QString localpath = chinfo->m_localIcon;
                m_channelImage->SetFilename(localpath);
                m_channelImage->Load();
            }
        }
    }

    pginfo->ToMap(infoMap);
    SetTextFromMap(infoMap);

    MythUIStateType *ratingState = dynamic_cast<MythUIStateType*>
                                                (GetChild("ratingstate"));
    if (ratingState)
    {
        QString rating = QString::number((int)((pginfo->stars * 10.0) + 0.5));
        ratingState->DisplayState(rating);
    }
}

void GuideGrid::toggleGuideListing()
{
    int oldchangrpid = m_changrpid;

    m_changrpid = ChannelGroup::GetNextChannelGroup(m_changrplist, oldchangrpid);

    if (oldchangrpid != m_changrpid)
      generateListings();

    updateChannels();
    updateInfo();

    QString changrpname = ChannelGroup::GetChannelGroupName(m_changrpid);

    if (m_changroupname)
        m_changroupname->SetText(changrpname);
    else
    {
        if (m_dateText)
        {
            m_dateText->SetText(changrpname);
            QTimer::singleShot(5000, this, SLOT(infoTimeout()));
        }
    }
}

void GuideGrid::generateListings()
{
    m_currentStartChannel = 0;
    m_currentRow = 0;

    int maxchannel = 0;
    fillChannelInfos();
    maxchannel = max((int)GetChannelCount() - 1, 0);
    m_channelCount = min(m_guideGrid->getChannelCount(), maxchannel + 1);

    m_recList.FromScheduler();
    fillProgramInfos();
}

void GuideGrid::ChannelGroupMenu(int mode)
{
    if (m_changrplist.empty())
    {
      QString message = tr("You don't have any channel groups defined");

      MythScreenStack *popupStack = GetMythMainWindow()->GetStack("popup stack");

      MythConfirmationDialog *okPopup = new MythConfirmationDialog(popupStack,
                                                                   message, false);
      if (okPopup->Create())
          popupStack->AddScreen(okPopup);
      else
          delete okPopup;

      return;
    }

    QString label = tr("Select Channel Group");

    MythScreenStack *popupStack = GetMythMainWindow()->GetStack("popup stack");
    MythDialogBox *menuPopup = new MythDialogBox(label, popupStack, "menuPopup");

    if (menuPopup->Create())
    {
        if (mode == 0)
            menuPopup->SetReturnEvent(this, "channelgrouptogglemenu");
        else
        {
            menuPopup->SetReturnEvent(this, "channelgroupmenu");
            menuPopup->AddButton(QObject::tr("All Channels"));
        }

        for (uint i = 0; i < m_changrplist.size(); i++)
            menuPopup->AddButton(m_changrplist[i].name);

        menuPopup->AddButton(tr("Cancel"));

        popupStack->AddScreen(menuPopup);
    }
    else
    {
        delete menuPopup;
    }
}

void GuideGrid::toggleChannelFavorite(int grpid)
{
    MSqlQuery query(MSqlQuery::InitCon());

    if (grpid == -1)
    {
      if (m_changrpid == -1)
          return;
      else
          grpid = m_changrpid;
    }

    // Get current channel id, and make sure it exists...
    int chanNum = m_currentRow + m_currentStartChannel;
    if (chanNum >= (int)m_channelInfos.size())
        chanNum -= (int)m_channelInfos.size();
    if (chanNum >= (int)m_channelInfos.size())
        return;
    if (chanNum < 0)
        chanNum = 0;

    PixmapChannel *ch = GetChannelInfo(chanNum);
    uint chanid = ch->chanid;

    if (m_changrpid == -1)
        // If currently viewing all channels, allow to add only not delete
        ChannelGroup::ToggleChannel(chanid, grpid, false);
    else
        // Only allow delete if viewing the favorite group in question
        ChannelGroup::ToggleChannel(chanid, grpid, true);

    // If viewing favorites, refresh because a channel was removed
    if (m_changrpid != -1)
    {
        generateListings();
        updateChannels();
        updateInfo();
    }
}

void GuideGrid::cursorLeft()
{
    ProgramInfo *test = m_programInfos[m_currentRow][m_currentCol];

    if (!test)
    {
        scrollLeft();
        return;
    }

    int startCol = test->startCol;
    m_currentCol = startCol - 1;

    if (m_currentCol < 0)
    {
        m_currentCol = 0;
        scrollLeft();
    }
    else
    {
        fillProgramRowInfos(m_currentRow);
        m_guideGrid->SetRedraw();
        updateInfo();
    }
}

void GuideGrid::cursorRight()
{
    ProgramInfo *test = m_programInfos[m_currentRow][m_currentCol];

    if (!test)
    {
        scrollRight();
        return;
    }

    int spread = test->spread;
    int startCol = test->startCol;

    m_currentCol = startCol + spread;

    if (m_currentCol > m_timeCount - 1)
    {
        m_currentCol = m_timeCount - 1;
        scrollRight();
    }
    else
    {
        fillProgramRowInfos(m_currentRow);
        m_guideGrid->SetRedraw();
        updateInfo();
    }
}

void GuideGrid::cursorDown()
{
    m_currentRow++;

    if (m_currentRow > m_channelCount - 1)
    {
        m_currentRow = m_channelCount - 1;
        scrollDown();
    }
    else
    {
        fillProgramRowInfos(m_currentRow);
        m_guideGrid->SetRedraw();
        updateInfo();
        updateChannels();
    }
}

void GuideGrid::cursorUp()
{
    m_currentRow--;

    if (m_currentRow < 0)
    {
        m_currentRow = 0;
        scrollUp();
    }
    else
    {
        fillProgramRowInfos(m_currentRow);
        m_guideGrid->SetRedraw();
        updateInfo();
        updateChannels();
    }
}

void GuideGrid::scrollLeft()
{
    bool updatedate = false;

    QDateTime t = m_currentStartTime;

    t = m_currentStartTime.addSecs(-30 * 60);

    if (t.date().day() != m_currentStartTime.date().day())
        updatedate = true;

    m_currentStartTime = t;

    fillTimeInfos();
    fillProgramInfos();
    m_guideGrid->SetRedraw();
    updateInfo();

    if (m_dateText)
        m_dateText->SetText(m_currentStartTime.toString(m_dateFormat));
}

void GuideGrid::scrollRight()
{
    bool updatedate = false;

    QDateTime t = m_currentStartTime;
    t = m_currentStartTime.addSecs(30 * 60);

    if (t.date().day() != m_currentStartTime.date().day())
        updatedate = true;

    m_currentStartTime = t;

    fillTimeInfos();
    fillProgramInfos();
    m_guideGrid->SetRedraw();
    updateInfo();

    if (m_dateText)
        m_dateText->SetText(m_currentStartTime.toString(m_dateFormat));
}

void GuideGrid::setStartChannel(int newStartChannel)
{
    if (newStartChannel < 0)
        m_currentStartChannel = newStartChannel + GetChannelCount();
    else if (newStartChannel >= (int) GetChannelCount())
        m_currentStartChannel = newStartChannel - GetChannelCount();
    else
        m_currentStartChannel = newStartChannel;
}

void GuideGrid::scrollDown()
{
    setStartChannel(m_currentStartChannel + 1);

    fillProgramInfos();
    m_guideGrid->SetRedraw();
    updateInfo();
    updateChannels();
}

void GuideGrid::scrollUp()
{
    setStartChannel((int)(m_currentStartChannel) - 1);

    fillProgramInfos();
    m_guideGrid->SetRedraw();
    updateInfo();
    updateChannels();
}

void GuideGrid::dayLeft()
{
    m_currentStartTime = m_currentStartTime.addSecs(-24 * 60 * 60);

    fillTimeInfos();
    fillProgramInfos();

    m_guideGrid->SetRedraw();
    updateInfo();

    if (m_dateText)
        m_dateText->SetText(m_currentStartTime.toString(m_dateFormat));
}

void GuideGrid::dayRight()
{
    m_currentStartTime = m_currentStartTime.addSecs(24 * 60 * 60);

    fillTimeInfos();
    fillProgramInfos();

    m_guideGrid->SetRedraw();
    updateInfo();

    if (m_dateText)
        m_dateText->SetText(m_currentStartTime.toString(m_dateFormat));
}

void GuideGrid::pageLeft()
{
    m_currentStartTime = m_currentStartTime.addSecs(-5 * 60 * m_timeCount);

    fillTimeInfos();
    fillProgramInfos();

    m_guideGrid->SetRedraw();
    updateInfo();

    if (m_dateText)
        m_dateText->SetText(m_currentStartTime.toString(m_dateFormat));
}

void GuideGrid::pageRight()
{
    m_currentStartTime = m_currentStartTime.addSecs(5 * 60 * m_timeCount);

    fillTimeInfos();
    fillProgramInfos();

    m_guideGrid->SetRedraw();
    updateInfo();

    if (m_dateText)
        m_dateText->SetText(m_currentStartTime.toString(m_dateFormat));
}

void GuideGrid::pageDown()
{
    setStartChannel(m_currentStartChannel + m_channelCount);

    fillProgramInfos();

    m_guideGrid->SetRedraw();
    updateInfo();
    updateChannels();
}

void GuideGrid::pageUp()
{
    setStartChannel((int)(m_currentStartChannel) - m_channelCount);

    fillProgramInfos();

    m_guideGrid->SetRedraw();
    updateInfo();
    updateChannels();
}

void GuideGrid::showProgFinder()
{
    if (m_allowFinder)
        RunProgramFinder(m_player, m_embedVideo, false);
}

void GuideGrid::enter()
{
    if (!m_player)
        return;

    if (m_updateTimer)
        m_updateTimer->stop();

    channelUpdate();

    // Don't perform transition effects when guide is being used during playback
    GetScreenStack()->PopScreen(this, false);

    epgIsVisibleCond.wakeAll();
}

void GuideGrid::escape()
{
    if (m_updateTimer)
        m_updateTimer->stop();

    // don't fade the screen if we are returning to the player
    if (m_player)
        GetScreenStack()->PopScreen(this, false);
    else
        GetScreenStack()->PopScreen(this, true);

    epgIsVisibleCond.wakeAll();
}

void GuideGrid::quickRecord()
{
    ProgramInfo *pginfo = m_programInfos[m_currentRow][m_currentCol];

    if (!pginfo)
        return;

    if (pginfo->title == m_unknownTitle)
        return;

    RecordingInfo ri(*pginfo);
    ri.ToggleRecord();
    *pginfo = ri;

    m_recList.FromScheduler();
    fillProgramInfos();
    updateInfo();
}

void GuideGrid::editRecording()
{
    ProgramInfo *pginfo = m_programInfos[m_currentRow][m_currentCol];

    if (!pginfo)
        return;

    if (pginfo->title == m_unknownTitle)
        return;

    RecordingInfo ri(*pginfo);
    ri.EditRecording();
    // we don't want to update pginfo, it will instead be updated
    // when the scheduler is done..

    m_recList.FromScheduler();
    fillProgramInfos();
    updateInfo();
}

void GuideGrid::editScheduled()
{
    ProgramInfo *pginfo = m_programInfos[m_currentRow][m_currentCol];

    if (!pginfo)
        return;

    if (pginfo->title == m_unknownTitle)
        return;

    RecordingInfo ri(*pginfo);
    ri.EditScheduled();
    // we don't want to update pginfo, it will instead be updated
    // when the scheduler is done..

    m_recList.FromScheduler();
    fillProgramInfos();
    updateInfo();
}

void GuideGrid::customEdit()
{
    ProgramInfo *pginfo = m_programInfos[m_currentRow][m_currentCol];

    if (!pginfo)
        return;

    CustomEdit *ce = new CustomEdit(gContext->GetMainWindow(),
                                    "customedit", pginfo);
    ce->exec();
    delete ce;
}

void GuideGrid::deleteRule()
{
    ProgramInfo *pginfo = m_programInfos[m_currentRow][m_currentCol];

    if (!pginfo || pginfo->recordid <= 0)
        return;

    ScheduledRecording *record = new ScheduledRecording();
    int recid = pginfo->recordid;
    record->loadByID(recid);

    QString message =
        tr("Delete '%1' %2 rule?").arg(record->getRecordTitle())
                                  .arg(pginfo->RecTypeText());

    MythScreenStack *popupStack = GetMythMainWindow()->GetStack("popup stack");

    MythConfirmationDialog *okPopup = new MythConfirmationDialog(popupStack,
                                                                 message, true);

    okPopup->SetReturnEvent(this, "deleterule");
    okPopup->SetData(qVariantFromValue(record));

    if (okPopup->Create())
        popupStack->AddScreen(okPopup);
    else
        delete okPopup;
}

void GuideGrid::upcoming()
{
    ProgramInfo *pginfo = m_programInfos[m_currentRow][m_currentCol];

    if (!pginfo)
        return;

    if (pginfo->title == m_unknownTitle)
        return;

    MythScreenStack *mainStack = GetMythMainWindow()->GetMainStack();
    ProgLister *pl = new ProgLister(mainStack, plTitle, pginfo->title, "");
    if (pl->Create())
        mainStack->AddScreen(pl);
    else
        delete pl;
}

void GuideGrid::details()
{
    ProgramInfo *pginfo = m_programInfos[m_currentRow][m_currentCol];

    if (!pginfo)
        return;

    if (pginfo->title == m_unknownTitle)
        return;

    const RecordingInfo ri(*pginfo);
    ri.showDetails();
}

void GuideGrid::channelUpdate(void)
{
    if (!m_player)
        return;

    DBChanList sel = GetSelection();

    if (sel.size())
    {
        PlayerContext *ctx = m_player->GetPlayerReadLock(-1, __FILE__, __LINE__);
        m_player->ChangeChannel(ctx, sel);
        m_player->ReturnPlayerLock(ctx);
    }
}

void GuideGrid::volumeUpdate(bool up)
{
    if (m_player)
    {
        PlayerContext *ctx = m_player->GetPlayerReadLock(-1, __FILE__, __LINE__);
        m_player->ChangeVolume(ctx, up);
        m_player->ReturnPlayerLock(ctx);
    }
}

void GuideGrid::toggleMute(void)
{
    if (m_player)
    {
        PlayerContext *ctx = m_player->GetPlayerReadLock(-1, __FILE__, __LINE__);
        m_player->ToggleMute(ctx);
        m_player->ReturnPlayerLock(ctx);
    }
}

void GuideGrid::GoTo(int start, int cur_row)
{
    setStartChannel(start);
    m_currentRow = cur_row % m_channelCount;
    updateChannels();
    fillProgramInfos();
    updateInfo();
    updateJumpToChannel();
}

void GuideGrid::updateJumpToChannel(void)
{
    QString txt = "";
    {
        QMutexLocker locker(&m_jumpToChannelLock);
        if (m_jumpToChannel)
            txt = m_jumpToChannel->GetEntry();
    }

    if (txt.isEmpty())
        return;

    if (m_jumpToText)
        m_jumpToText->SetText(txt);
    else if (m_dateText)
        m_dateText->SetText(txt);
}

void GuideGrid::SetJumpToChannel(JumpToChannel *ptr)
{
    QMutexLocker locker(&m_jumpToChannelLock);
    m_jumpToChannel = ptr;

    if (!m_jumpToChannel)
    {
        if (m_jumpToText)
            m_jumpToText->Reset();

        if (m_dateText)
            m_dateText->SetText(m_currentStartTime.toString(m_dateFormat));
    }
}

void GuideGrid::HideTVWindow(void)
{
    GetMythMainWindow()->GetPaintWindow()->clearMask();
}

void GuideGrid::EmbedTVWindow(void)
{
    previewVideoRefreshTimer->stop();
    if (m_embedVideo)
    {
        PlayerContext *ctx =
            m_player->GetPlayerReadLock(-1, __FILE__, __LINE__);
        m_usingNullVideo =
            !m_player->StartEmbedding(ctx, GetMythMainWindow()->GetPaintWindow()->winId(), m_videoRect);
        if (!m_usingNullVideo)
        {
            QRegion r1 = QRegion(m_Area);
            QRegion r2 = QRegion(m_videoRect);
            GetMythMainWindow()->GetPaintWindow()->setMask(r1.xored(r2));
            m_player->DrawUnusedRects(false, ctx);
        }
        else
        {
            previewVideoRefreshTimer->start(66);
        }
        m_player->ReturnPlayerLock(ctx);
    }
}

void GuideGrid::refreshVideo(void)
{
    if (m_player && m_player->IsRunning() && m_usingNullVideo)
    {
        GetMythMainWindow()->GetPaintWindow()->update(m_videoRect);
    }
}

void GuideGrid::aboutToHide(void)
{
    if (m_player)
        HideTVWindow();

    MythScreenType::aboutToHide();
}

void GuideGrid::aboutToShow(void)
{
    if (m_player && m_player->IsRunning())
        EmbedTVWindow();

    MythScreenType::aboutToShow();
}
