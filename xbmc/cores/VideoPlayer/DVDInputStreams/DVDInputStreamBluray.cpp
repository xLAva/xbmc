/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include <functional>
#include <limits>

#include "filesystem/BlurayCallback.h"
#include "DVDInputStreamBluray.h"
#include "IVideoPlayer.h"
#include "DVDCodecs/Overlay/DVDOverlay.h"
#include "DVDCodecs/Overlay/DVDOverlayImage.h"
#include "settings/Settings.h"
#include "LangInfo.h"
#include "ServiceBroker.h"
#include "utils/log.h"
#include "utils/URIUtils.h"
#include "filesystem/File.h"
#include "filesystem/Directory.h"
#include "URL.h"
#include "utils/Geometry.h"
#include "guilib/LocalizeStrings.h"
#include "settings/DiscSettings.h"
#include "utils/LangCodeExpander.h"
#include "filesystem/SpecialProtocol.h"
#include "utils/StringUtils.h"

#ifdef TARGET_POSIX
#include "platform/linux/XTimeUtils.h"
#endif

#include <libbluray/bluray.h>
#include <libbluray/log_control.h>

#define LIBBLURAY_BYTESEEK 0

using namespace XFILE;


static int read_blocks(void* handle, void* buf, int lba, int num_blocks)
{
  int result = -1;
  CDVDInputStreamFile* lpstream = reinterpret_cast<CDVDInputStreamFile*>(handle);
  int64_t offset = static_cast<int64_t>(lba) * 2048;
  if (lpstream->Seek(offset, SEEK_SET) >= 0)
  {
    int64_t size = static_cast<int64_t>(num_blocks) * 2048;
    if (size <= std::numeric_limits<int>::max())
      result = lpstream->Read(reinterpret_cast<uint8_t*>(buf), static_cast<int>(size)) / 2048;
  }

  return result;
}

static void bluray_overlay_cb(void *this_gen, const BD_OVERLAY * ov)
{
  static_cast<CDVDInputStreamBluray*>(this_gen)->OverlayCallback(ov);
}

#ifdef HAVE_LIBBLURAY_BDJ
void  bluray_overlay_argb_cb(void *this_gen, const struct bd_argb_overlay_s * const ov)
{
  static_cast<CDVDInputStreamBluray*>(this_gen)->OverlayCallbackARGB(ov);
}
#endif

CDVDInputStreamBluray::CDVDInputStreamBluray(IVideoPlayer* player, const CFileItem& fileitem) :
  CDVDInputStream(DVDSTREAM_TYPE_BLURAY, fileitem), m_pstream(nullptr), m_rootPath("")
{
  m_title = NULL;
  m_clip  = (uint32_t)-1;
  m_angle = 0;
  m_playlist = (uint32_t)-1;
  m_menu  = false;
  m_bd    = NULL;
  m_content = "video/x-mpegts";
  m_player  = player;
  m_navmode = false;
  m_hold = HOLD_NONE;
  m_angle = 0;
  memset(&m_event, 0, sizeof(m_event));
#ifdef HAVE_LIBBLURAY_BDJ
  memset(&m_argb,  0, sizeof(m_argb));
#endif
}

CDVDInputStreamBluray::~CDVDInputStreamBluray()
{
  Close();
}

void CDVDInputStreamBluray::Abort()
{
  m_hold = HOLD_EXIT;
}

bool CDVDInputStreamBluray::IsEOF()
{
  return false;
}

BLURAY_TITLE_INFO* CDVDInputStreamBluray::GetTitleLongest()
{
  int titles = bd_get_titles(m_bd, TITLES_RELEVANT, 0);

  BLURAY_TITLE_INFO *s = NULL;
  for(int i=0; i < titles; i++)
  {
    BLURAY_TITLE_INFO *t = bd_get_title_info(m_bd, i, 0);
    if(!t)
    {
      CLog::Log(LOGDEBUG, "get_main_title - unable to get title %d", i);
      continue;
    }
    if(!s || s->duration < t->duration)
      std::swap(s, t);

    if(t)
      bd_free_title_info(t);
  }
  return s;
}

BLURAY_TITLE_INFO* CDVDInputStreamBluray::GetTitleFile(const std::string& filename)
{
  unsigned int playlist;
  if(sscanf(filename.c_str(), "%05u.mpls", &playlist) != 1)
  {
    CLog::Log(LOGERROR, "get_playlist_title - unsupported playlist file selected %s", CURL::GetRedacted(filename).c_str());
    return NULL;
  }

  return bd_get_playlist_info(m_bd, playlist, 0);
}


bool CDVDInputStreamBluray::Open()
{
  if(m_player == NULL)
    return false;

  std::string strPath(m_item.GetPath());
  std::string filename;
  std::string root;

  bool openStream = false;

  // The item was selected via the simple menu
  if (URIUtils::IsProtocol(strPath, "bluray"))
  {
    CURL url(strPath);
    root = url.GetHostName();
    filename = URIUtils::GetFileName(url.GetFileName());

    // check for a menu call for an image file
    if (StringUtils::EqualsNoCase(filename, "menu"))
    {
      //get rid of the udf:// protocol
      CURL url2(root);
      std::string root2 = url2.GetHostName();
      CURL url(root2);
      CFileItem item(url, false);
      if (item.IsDiscImage())
      {
        if (!OpenStream(item))
          return false;

        openStream = true;
      }
    }
  }
  else if (m_item.IsDiscImage())
  {
    if (!OpenStream(m_item))
      return false;

    openStream = true;
  }
  else
  {
    strPath = URIUtils::GetDirectory(strPath);
    URIUtils::RemoveSlashAtEnd(strPath);

    if(URIUtils::GetFileName(strPath) == "PLAYLIST")
    {
      strPath = URIUtils::GetDirectory(strPath);
      URIUtils::RemoveSlashAtEnd(strPath);
    }

    if(URIUtils::GetFileName(strPath) == "BDMV")
    {
      strPath = URIUtils::GetDirectory(strPath);
      URIUtils::RemoveSlashAtEnd(strPath);
    }
    root = strPath;
    filename = URIUtils::GetFileName(m_item.GetPath());
  }

  // root should not have trailing slash
  URIUtils::RemoveSlashAtEnd(root);

  bd_set_debug_handler(CBlurayCallback::bluray_logger);
  bd_set_debug_mask(DBG_CRIT | DBG_BLURAY | DBG_NAV);

  m_bd = bd_init();

  if (!m_bd)
  {
    CLog::Log(LOGERROR, "CDVDInputStreamBluray::Open - failed to initialize libbluray");
    return false;
  }

  SetupPlayerSettings();

  CLog::Log(LOGDEBUG, "CDVDInputStreamBluray::Open - opening %s", CURL::GetRedacted(root).c_str());

  if (openStream)
  {
    if (!bd_open_stream(m_bd, m_pstream.get(), read_blocks))
    {
      CLog::Log(LOGERROR, "CDVDInputStreamBluray::Open - failed to open %s in stream mode", CURL::GetRedacted(root).c_str());
      return false;
    }
  }
  else
  {
    m_rootPath = root;
    if (!bd_open_files(m_bd, &m_rootPath, CBlurayCallback::dir_open, CBlurayCallback::file_open))
    {
      CLog::Log(LOGERROR, "CDVDInputStreamBluray::Open - failed to open %s", CURL::GetRedacted(root).c_str());
      return false;
    }
  }

  bd_get_event(m_bd, NULL);

  const BLURAY_DISC_INFO *disc_info = bd_get_disc_info(m_bd);

  if (!disc_info)
  {
    CLog::Log(LOGERROR, "CDVDInputStreamBluray::Open - bd_get_disc_info() failed");
    return false;
  }

  if (disc_info->bluray_detected)
  {
#if (BLURAY_VERSION > BLURAY_VERSION_CODE(1,0,0))
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray::Open - Disc name           : %s", disc_info->disc_name ? disc_info->disc_name : "");
#endif
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray::Open - First Play supported: %d", disc_info->first_play_supported);
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray::Open - Top menu supported  : %d", disc_info->top_menu_supported);
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray::Open - HDMV titles         : %d", disc_info->num_hdmv_titles);
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray::Open - BD-J titles         : %d", disc_info->num_bdj_titles);
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray::Open - BD-J handled        : %d", disc_info->bdj_handled);
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray::Open - UNSUPPORTED titles  : %d", disc_info->num_unsupported_titles);
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray::Open - AACS detected       : %d", disc_info->aacs_detected);
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray::Open - libaacs detected    : %d", disc_info->libaacs_detected);
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray::Open - AACS handled        : %d", disc_info->aacs_handled);
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray::Open - BD+ detected        : %d", disc_info->bdplus_detected);
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray::Open - libbdplus detected  : %d", disc_info->libbdplus_detected);
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray::Open - BD+ handled         : %d", disc_info->bdplus_handled);
#if (BLURAY_VERSION >= BLURAY_VERSION_CODE(1,0,0))
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray::Open - no menus (libmmbd)  : %d", disc_info->no_menu_support);
#endif
  }
  else
    CLog::Log(LOGERROR, "CDVDInputStreamBluray::Open - BluRay not detected");

  if (disc_info->aacs_detected && !disc_info->aacs_handled)
  {
    CLog::Log(LOGERROR, "CDVDInputStreamBluray::Open - Media stream scrambled/encrypted with AACS");
    m_player->OnDiscNavResult(nullptr, BD_EVENT_ENC_ERROR);
    return false;
  }

  if (disc_info->bdplus_detected && !disc_info->bdplus_handled)
  {
    CLog::Log(LOGERROR, "CDVDInputStreamBluray::Open - Media stream scrambled/encrypted with BD+");
    m_player->OnDiscNavResult(nullptr, BD_EVENT_ENC_ERROR);
    return false;
  }

  int mode = CServiceBroker::GetSettings().GetInt(CSettings::SETTING_DISC_PLAYBACK);

  if (URIUtils::HasExtension(filename, ".mpls"))
  {
    m_navmode = false;
    m_title = GetTitleFile(filename);
  }
  else if (mode == BD_PLAYBACK_MAIN_TITLE)
  {
    m_navmode = false;
    m_title = GetTitleLongest();
  }
  else
  {
    m_navmode = true;
    if (m_navmode && !disc_info->first_play_supported) {
      CLog::Log(LOGERROR, "CDVDInputStreamBluray::Open - Can't play disc in HDMV navigation mode - First Play title not supported");
      m_navmode = false;
    }

    if (m_navmode && disc_info->num_unsupported_titles > 0) {
      CLog::Log(LOGERROR, "CDVDInputStreamBluray::Open - Unsupported titles found - Some titles can't be played in navigation mode");
    }

    if(!m_navmode)
      m_title = GetTitleLongest();
  }

  if (m_navmode)
  {

    bd_register_overlay_proc (m_bd, this, bluray_overlay_cb);
#ifdef HAVE_LIBBLURAY_BDJ
    bd_register_argb_overlay_proc (m_bd, this, bluray_overlay_argb_cb, NULL);
#endif

    if(bd_play(m_bd) <= 0)
    {
      CLog::Log(LOGERROR, "CDVDInputStreamBluray::Open - failed play disk %s", CURL::GetRedacted(strPath).c_str());
      return false;
    }
    m_hold = HOLD_DATA;
  }
  else
  {
    if(!m_title)
    {
      CLog::Log(LOGERROR, "CDVDInputStreamBluray::Open - failed to get title info");
      return false;
    }

    if(bd_select_playlist(m_bd, m_title->playlist) == 0 )
    {
      CLog::Log(LOGERROR, "CDVDInputStreamBluray::Open - failed to select title %d", m_title->idx);
      return false;
    }
    m_clip = 0;
  }

  // Process any events that occurred during opening
  while (bd_get_event(m_bd, &m_event))
    ProcessEvent();

  return true;
}

// close file and reset everything
void CDVDInputStreamBluray::Close()
{
  if(m_title)
    bd_free_title_info(m_title);
  if(m_bd)
  {
    bd_register_overlay_proc(m_bd, NULL, NULL);
    bd_close(m_bd);
  }
  m_bd = NULL;
  m_title = NULL;
  m_pstream.reset();
  m_rootPath.clear();
}

void CDVDInputStreamBluray::ProcessEvent() {

  int pid = -1;
  switch (m_event.event) {

   /* errors */

  case BD_EVENT_ERROR:
    switch (m_event.param)
    {
    case BD_ERROR_HDMV:
    case BD_ERROR_BDJ:
      m_player->OnDiscNavResult(nullptr, BD_EVENT_MENU_ERROR);
      break;
    default:
      break;
    }
    CLog::Log(LOGERROR, "CDVDInputStreamBluray - BD_EVENT_ERROR: Fatal error. Playback can't be continued.");
    m_hold = HOLD_ERROR;
    break;

  case BD_EVENT_READ_ERROR:
    CLog::Log(LOGERROR, "CDVDInputStreamBluray - BD_EVENT_READ_ERROR");
    break;

  case BD_EVENT_ENCRYPTED:
    CLog::Log(LOGERROR, "CDVDInputStreamBluray - BD_EVENT_ENCRYPTED");
    switch (m_event.param)
    {
    case BD_ERROR_AACS:
      CLog::Log(LOGERROR, "CDVDInputStreamBluray - BD_ERROR_AACS");
      break;
    case BD_ERROR_BDPLUS:
      CLog::Log(LOGERROR, "CDVDInputStreamBluray - BD_ERROR_BDPLUS");
      break;
    default:
      break;
    }
    m_hold = HOLD_ERROR;
    m_player->OnDiscNavResult(nullptr, BD_EVENT_ENC_ERROR);
    break;

  /* playback control */

  case BD_EVENT_SEEK:
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray - BD_EVENT_SEEK");
    //m_player->OnDVDNavResult(NULL, 1);
    //bd_read_skip_still(m_bd);
    //m_hold = HOLD_HELD;
    break;

  case BD_EVENT_STILL_TIME:
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray - BD_EVENT_STILL_TIME %d", m_event.param);
    pid = m_event.param;
    m_player->OnDiscNavResult(static_cast<void*>(&pid), BD_EVENT_STILL_TIME);
    m_hold = HOLD_STILL;
    break;

  case BD_EVENT_STILL:
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray - BD_EVENT_STILL %d",
        m_event.param);
    break;

    /* playback position */

  case BD_EVENT_ANGLE:
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray - BD_EVENT_ANGLE %d",
        m_event.param);
    m_angle = m_event.param;

    if (m_playlist <= MAX_PLAYLIST_ID)
    {
      if(m_title)
        bd_free_title_info(m_title);
      m_title = bd_get_playlist_info(m_bd, m_playlist, m_angle);
    }
    break;

  case BD_EVENT_END_OF_TITLE:
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray - BD_EVENT_END_OF_TITLE %d",
        m_event.param);
    /* when a title ends, playlist WILL eventually change */
    if (m_title)
      bd_free_title_info(m_title);
    m_title = NULL;
    break;

  case BD_EVENT_TITLE:
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray - BD_EVENT_TITLE %d",
        m_event.param);
    break;

  case BD_EVENT_PLAYLIST:
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray - BD_EVENT_PLAYLIST %d",
        m_event.param);
    m_playlist = m_event.param;
    if(m_title)
      bd_free_title_info(m_title);
    m_title = bd_get_playlist_info(m_bd, m_playlist, m_angle);
    break;

  case BD_EVENT_PLAYITEM:
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray - BD_EVENT_PLAYITEM %d",
        m_event.param);
    m_clip    = m_event.param;
    break;

  case BD_EVENT_CHAPTER:
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray - BD_EVENT_CHAPTER %d",
        m_event.param);
    break;

    /* stream selection */

  case BD_EVENT_AUDIO_STREAM:
    pid = -1;
    if (m_title && m_title->clip_count > m_clip
        && m_title->clips[m_clip].audio_stream_count
            > (uint8_t) (m_event.param - 1))
      pid = m_title->clips[m_clip].audio_streams[m_event.param - 1].pid;
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray - BD_EVENT_AUDIO_STREAM %d %d",
        m_event.param, pid);
    m_player->OnDiscNavResult(static_cast<void*>(&pid), BD_EVENT_AUDIO_STREAM);
    break;

  case BD_EVENT_PG_TEXTST:
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray - BD_EVENT_PG_TEXTST %d",
        m_event.param);
    pid = m_event.param;
    m_player->OnDiscNavResult(static_cast<void*>(&pid), BD_EVENT_PG_TEXTST);
    break;

  case BD_EVENT_PG_TEXTST_STREAM:
    pid = -1;
    if (m_title && m_title->clip_count > m_clip
        && m_title->clips[m_clip].pg_stream_count
            > (uint8_t) (m_event.param - 1))
      pid = m_title->clips[m_clip].pg_streams[m_event.param - 1].pid;
    CLog::Log(LOGDEBUG,
        "CDVDInputStreamBluray - BD_EVENT_PG_TEXTST_STREAM %d, %d",
        m_event.param, pid);
    m_player->OnDiscNavResult(static_cast<void*>(&pid), BD_EVENT_PG_TEXTST_STREAM);
    break;

  case BD_EVENT_MENU:
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray - BD_EVENT_MENU %d",
        m_event.param);
    m_menu = (m_event.param != 0);
    break;

  case BD_EVENT_IDLE:
    Sleep(100);
    break;

  case BD_EVENT_SOUND_EFFECT:
  {
    BLURAY_SOUND_EFFECT effect;
    if (bd_get_sound_effect(m_bd, m_event.param, &effect) <= 0)
    {
      CLog::Log(LOGDEBUG, "CDVDInputStreamBluray - BD_EVENT_SOUND_EFFECT %d not valid",
        m_event.param);
    }
    else
    {
      CLog::Log(LOGDEBUG, "CDVDInputStreamBluray - BD_EVENT_SOUND_EFFECT %d",
        m_event.param);
    }
  }

  case BD_EVENT_IG_STREAM:
  case BD_EVENT_SECONDARY_AUDIO:
  case BD_EVENT_SECONDARY_AUDIO_STREAM:
  case BD_EVENT_SECONDARY_VIDEO:
  case BD_EVENT_SECONDARY_VIDEO_SIZE:
  case BD_EVENT_SECONDARY_VIDEO_STREAM:
  case BD_EVENT_PLAYMARK:
    break;

  case BD_EVENT_PLAYLIST_STOP:
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray - BD_EVENT_PLAYLIST_STOP: flush buffers");
    m_player->OnDiscNavResult(nullptr, BD_EVENT_PLAYLIST_STOP);
    break;
  case BD_EVENT_NONE:
    break;

  default:
    CLog::Log(LOGWARNING,
        "CDVDInputStreamBluray - unhandled libbluray event %d [param %d]",
        m_event.event, m_event.param);
    break;
  }

  /* event has been consumed */
  m_event.event = BD_EVENT_NONE;
}

int CDVDInputStreamBluray::Read(uint8_t* buf, int buf_size)
{
  int result = 0;
  m_dispTimeBeforeRead = (int)(bd_tell_time(m_bd) / 90);
  if(m_navmode)
  {
    do {

      if(m_hold == HOLD_HELD)
        return 0;

      if(  m_hold == HOLD_ERROR
        || m_hold == HOLD_EXIT)
        return -1;

      result = bd_read_ext (m_bd, buf, buf_size, &m_event);

      if(result < 0)
      {
        m_hold = HOLD_ERROR;
        return result;
      }

      /* Check for holding events */
      switch(m_event.event) {
        case BD_EVENT_SEEK:
        case BD_EVENT_TITLE:
        case BD_EVENT_ANGLE:
        case BD_EVENT_PLAYLIST:
        case BD_EVENT_PLAYITEM:
          if(m_hold != HOLD_DATA)
          {
            m_hold = HOLD_HELD;
            return result;
          }
          break;

        case BD_EVENT_STILL_TIME:
          if(m_hold == HOLD_STILL)
            m_event.event = 0; /* Consume duplicate still event */
          else
            m_hold = HOLD_HELD;
          return result;

        default:
          break;
      }

      if(result > 0)
        m_hold = HOLD_NONE;

      ProcessEvent();

    } while(result == 0);

  }
  else
  {
    result = bd_read(m_bd, buf, buf_size);
    while (bd_get_event(m_bd, &m_event))
      ProcessEvent();
  }
  return result;
}

static uint8_t  clamp(double v)
{
  return (v) > 255.0 ? 255 : ((v) < 0.0 ? 0 : (uint8_t)(v+0.5f));
}

static uint32_t build_rgba(const BD_PG_PALETTE_ENTRY &e)
{
  double r = 1.164 * (e.Y - 16)                        + 1.596 * (e.Cr - 128);
  double g = 1.164 * (e.Y - 16) - 0.391 * (e.Cb - 128) - 0.813 * (e.Cr - 128);
  double b = 1.164 * (e.Y - 16) + 2.018 * (e.Cb - 128);
  return (uint32_t)e.T      << PIXEL_ASHIFT
       | (uint32_t)clamp(r) << PIXEL_RSHIFT
       | (uint32_t)clamp(g) << PIXEL_GSHIFT
       | (uint32_t)clamp(b) << PIXEL_BSHIFT;
}

void CDVDInputStreamBluray::OverlayClose()
{
#if(BD_OVERLAY_INTERFACE_VERSION >= 2)
  for(unsigned i = 0; i < 2; ++i)
    m_planes[i].o.clear();
  CDVDOverlayGroup* group   = new CDVDOverlayGroup();
  group->bForced = true;
  m_player->OnDiscNavResult(static_cast<void*>(group), BD_EVENT_MENU_OVERLAY);
  group->Release();
#endif
}

void CDVDInputStreamBluray::OverlayInit(SPlane& plane, int w, int h)
{
#if(BD_OVERLAY_INTERFACE_VERSION >= 2)
  plane.o.clear();
  plane.w = w;
  plane.h = h;
#endif
}

void CDVDInputStreamBluray::OverlayClear(SPlane& plane, int x, int y, int w, int h)
{
#if(BD_OVERLAY_INTERFACE_VERSION >= 2)
  CRectInt ovr(x
          , y
          , x + w
          , y + h);

  /* fixup existing overlays */
  for(SOverlays::iterator it = plane.o.begin(); it != plane.o.end();)
  {
    CRectInt old((*it)->x
            , (*it)->y
            , (*it)->x + (*it)->width
            , (*it)->y + (*it)->height);

    std::vector<CRectInt> rem = old.SubtractRect(ovr);

    /* if no overlap we are done */
    if(rem.size() == 1 && !(rem[0] != old))
    {
      ++it;
      continue;
    }

    SOverlays add;
    for(std::vector<CRectInt>::iterator itr = rem.begin(); itr != rem.end(); ++itr)
    {
      SOverlay overlay(new CDVDOverlayImage(*(*it)
                                            , itr->x1
                                            , itr->y1
                                            , itr->Width()
                                            , itr->Height())
                    , [](CDVDOverlay* ov) { ov->Release(); });
      add.push_back(overlay);
    }

    it = plane.o.erase(it);
    plane.o.insert(it, add.begin(), add.end());
  }
#endif
}

void CDVDInputStreamBluray::OverlayFlush(int64_t pts)
{
#if(BD_OVERLAY_INTERFACE_VERSION >= 2)
  CDVDOverlayGroup* group   = new CDVDOverlayGroup();
  group->bForced       = true;
  group->iPTSStartTime = (double) pts;
  group->iPTSStopTime  = 0;

  for(unsigned i = 0; i < 2; ++i)
  {
    for(SOverlays::iterator it = m_planes[i].o.begin(); it != m_planes[i].o.end(); ++it)
      group->m_overlays.push_back((*it)->Acquire());
  }

  m_player->OnDiscNavResult(static_cast<void*>(group), BD_EVENT_MENU_OVERLAY);
  group->Release();
#endif
}

void CDVDInputStreamBluray::OverlayCallback(const BD_OVERLAY * const ov)
{
#if(BD_OVERLAY_INTERFACE_VERSION >= 2)
  if(ov == NULL || ov->cmd == BD_OVERLAY_CLOSE)
  {
    OverlayClose();
    return;
  }

  if (ov->plane > 1)
  {
    CLog::Log(LOGWARNING, "CDVDInputStreamBluray - Ignoring overlay with multiple planes");
    return;
  }

  SPlane& plane(m_planes[ov->plane]);

  if (ov->cmd == BD_OVERLAY_CLEAR)
  {
    plane.o.clear();
    return;
  }

  if (ov->cmd == BD_OVERLAY_INIT)
  {
    OverlayInit(plane, ov->w, ov->h);
    return;
  }

  if (ov->cmd == BD_OVERLAY_DRAW
  ||  ov->cmd == BD_OVERLAY_WIPE)
    OverlayClear(plane, ov->x, ov->y, ov->w, ov->h);


  /* uncompress and draw bitmap */
  if (ov->img && ov->cmd == BD_OVERLAY_DRAW)
  {
    SOverlay overlay(new CDVDOverlayImage(), [](CDVDOverlay* ov) { ov->Release(); });

    if (ov->palette)
    {
      overlay->palette_colors = 256;
      overlay->palette        = (uint32_t*)calloc(overlay->palette_colors, 4);

      for(unsigned i = 0; i < 256; i++)
        overlay->palette[i] = build_rgba(ov->palette[i]);
    }

    const BD_PG_RLE_ELEM *rlep = ov->img;
    uint8_t *img = (uint8_t*) malloc((size_t)ov->w * (size_t)ov->h);
    if (!img)
      return;
    unsigned pixels = ov->w * ov->h;

    for (unsigned i = 0; i < pixels; i += rlep->len, rlep++) {
      memset(img + i, rlep->color, rlep->len);
    }

    overlay->data     = img;
    overlay->linesize = ov->w;
    overlay->x        = ov->x;
    overlay->y        = ov->y;
    overlay->height   = ov->h;
    overlay->width    = ov->w;
    overlay->source_height = plane.h;
    overlay->source_width  = plane.w;
    plane.o.push_back(overlay);
  }

  if(ov->cmd == BD_OVERLAY_FLUSH)
    OverlayFlush(ov->pts);
#endif
}

#ifdef HAVE_LIBBLURAY_BDJ
void CDVDInputStreamBluray::OverlayCallbackARGB(const struct bd_argb_overlay_s * const ov)
{
  if(ov == NULL || ov->cmd == BD_ARGB_OVERLAY_CLOSE)
  {
    OverlayClose();
    return;
  }

  if (ov->plane > 1)
  {
    CLog::Log(LOGWARNING, "CDVDInputStreamBluray - Ignoring overlay with multiple planes");
    return;
  }

  SPlane& plane(m_planes[ov->plane]);

  if (ov->cmd == BD_ARGB_OVERLAY_INIT)
  {
    OverlayInit(plane, ov->w, ov->h);
    return;
  }

  if (ov->cmd == BD_ARGB_OVERLAY_DRAW)
    OverlayClear(plane, ov->x, ov->y, ov->w, ov->h);

  /* uncompress and draw bitmap */
  if (ov->argb && ov->cmd == BD_ARGB_OVERLAY_DRAW)
  {
    SOverlay overlay(new CDVDOverlayImage(), std::ptr_fun(CDVDOverlay::Release));

    overlay->palette_colors = 0;
    overlay->palette        = NULL;

    unsigned bytes = ov->stride * ov->h * 4;
    uint8_t *img = (uint8_t*) malloc(bytes);
    memcpy(img, ov->argb, bytes);

    overlay->data     = img;
    overlay->linesize = ov->stride * 4;
    overlay->x        = ov->x;
    overlay->y        = ov->y;
    overlay->height   = ov->h;
    overlay->width    = ov->w;
    overlay->source_height = plane.h;
    overlay->source_width  = plane.w;
    plane.o.push_back(overlay);
  }

  if(ov->cmd == BD_ARGB_OVERLAY_FLUSH)
    OverlayFlush(ov->pts);
}
#endif


int CDVDInputStreamBluray::GetTotalTime()
{
  if(m_title)
    return (int)(m_title->duration / 90);
  else
    return 0;
}

int CDVDInputStreamBluray::GetTime()
{
  return m_dispTimeBeforeRead;
}

bool CDVDInputStreamBluray::PosTime(int ms)
{
  if(bd_seek_time(m_bd, ms * 90) < 0)
    return false;

  while (bd_get_event(m_bd, &m_event))
    ProcessEvent();

  return true;
}

int CDVDInputStreamBluray::GetChapterCount()
{
  if(m_title)
    return m_title->chapter_count;
  else
    return 0;
}

int CDVDInputStreamBluray::GetChapter()
{
  if(m_title)
    return bd_get_current_chapter(m_bd) + 1;
  else
    return 0;
}

bool CDVDInputStreamBluray::SeekChapter(int ch)
{
  if(m_title && bd_seek_chapter(m_bd, ch-1) < 0)
    return false;

  while (bd_get_event(m_bd, &m_event))
    ProcessEvent();

  return true;
}

int64_t CDVDInputStreamBluray::GetChapterPos(int ch)
{
  if (ch == -1 || ch > GetChapterCount())
    ch = GetChapter();

  if (m_title && m_title->chapters)
    return m_title->chapters[ch - 1].start / 90000;
  else
    return 0;
}

int64_t CDVDInputStreamBluray::Seek(int64_t offset, int whence)
{
#if LIBBLURAY_BYTESEEK
  if(whence == SEEK_POSSIBLE)
    return 1;
  else if(whence == SEEK_CUR)
  {
    if(offset == 0)
      return bd_tell(m_bd);
    else
      offset += bd_tell(m_bd);
  }
  else if(whence == SEEK_END)
    offset += bd_get_title_size(m_bd);
  else if(whence != SEEK_SET)
    return -1;

  int64_t pos = bd_seek(m_bd, offset);
  if(pos < 0)
  {
    CLog::Log(LOGERROR, "CDVDInputStreamBluray::Seek - seek to %" PRId64", failed with %" PRId64, offset, pos);
    return -1;
  }

  if(pos != offset)
    CLog::Log(LOGWARNING, "CDVDInputStreamBluray::Seek - seek to %" PRId64", ended at %" PRId64, offset, pos);

  return offset;
#else
  if(whence == SEEK_POSSIBLE)
    return 0;
  return -1;
#endif
}

int64_t CDVDInputStreamBluray::GetLength()
{
  return bd_get_title_size(m_bd);
}

static bool find_stream(int pid, BLURAY_STREAM_INFO *info, int count, std::string &language)
{
  int i=0;
  for(;i<count;i++,info++)
  {
    if(info->pid == pid)
      break;
  }
  if(i==count)
    return false;
  language = reinterpret_cast<char*>(info->lang);
  return true;
}

void CDVDInputStreamBluray::GetStreamInfo(int pid, std::string &language)
{
  if(!m_title || m_clip >= m_title->clip_count)
    return;

  BLURAY_CLIP_INFO *clip = m_title->clips+m_clip;

  if(find_stream(pid, clip->audio_streams, clip->audio_stream_count, language))
    return;
  if(find_stream(pid, clip->video_streams, clip->video_stream_count, language))
    return;
  if(find_stream(pid, clip->pg_streams, clip->pg_stream_count, language))
    return;
  if(find_stream(pid, clip->ig_streams, clip->ig_stream_count, language))
    return;
}

CDVDInputStream::ENextStream CDVDInputStreamBluray::NextStream()
{
  if(!m_navmode || m_hold == HOLD_EXIT || m_hold == HOLD_ERROR)
    return NEXTSTREAM_NONE;

  /* process any current event */
  ProcessEvent();

  /* process all queued up events */
  while(bd_get_event(m_bd, &m_event))
    ProcessEvent();

  if(m_hold == HOLD_STILL)
    return NEXTSTREAM_RETRY;

  m_hold = HOLD_DATA;
  return NEXTSTREAM_OPEN;
}

void CDVDInputStreamBluray::UserInput(bd_vk_key_e vk)
{
  if(m_bd == NULL || !m_navmode)
    return;

  int ret = bd_user_input(m_bd, -1, vk);
  if (ret < 0)
  {
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray::UserInput - user input failed");
  }
  else
  {
    /* process all queued up events */
    while (bd_get_event(m_bd, &m_event))
      ProcessEvent();
  }
}

bool CDVDInputStreamBluray::MouseMove(const CPoint &point)
{
  if (m_bd == NULL || !m_navmode)
    return false;

  if (bd_mouse_select(m_bd, -1, (uint16_t)point.x, (uint16_t)point.y) < 0)
  {
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray::MouseMove - mouse select failed");
    return false;
  }

  return true;
}

bool CDVDInputStreamBluray::MouseClick(const CPoint &point)
{
  if (m_bd == NULL || !m_navmode)
    return false;

  if (bd_mouse_select(m_bd, -1, (uint16_t)point.x, (uint16_t)point.y) < 0)
  {
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray::MouseClick - mouse select failed");
    return false;
  }

  if (bd_user_input(m_bd, -1, BD_VK_MOUSE_ACTIVATE) >= 0)
    return true;

  CLog::Log(LOGDEBUG, "CDVDInputStreamBluray::MouseClick - mouse click (user input) failed");
  return false;
}

void CDVDInputStreamBluray::OnMenu()
{
  if(m_bd == NULL || !m_navmode)
  {
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray::OnMenu - navigation mode not enabled");
    return;
  }

  if(bd_user_input(m_bd, -1, BD_VK_POPUP) >= 0)
    return;
  CLog::Log(LOGDEBUG, "CDVDInputStreamBluray::OnMenu - popup failed, trying root");

  if(bd_user_input(m_bd, -1, BD_VK_ROOT_MENU) >= 0)
    return;

  CLog::Log(LOGDEBUG, "CDVDInputStreamBluray::OnMenu - root failed, trying explicit");
  if(bd_menu_call(m_bd, -1) <= 0)
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray::OnMenu - root failed");
}

bool CDVDInputStreamBluray::IsInMenu()
{
  if(m_bd == NULL || !m_navmode)
    return false;
  if(m_menu || !m_planes[BD_OVERLAY_IG].o.empty())
    return true;
  return false;
}

void CDVDInputStreamBluray::SkipStill()
{
  if(m_bd == NULL || !m_navmode)
    return;

  if(m_hold == HOLD_STILL)
  {
    m_hold = HOLD_HELD;
    bd_read_skip_still(m_bd);

    /* process all queued up events */
    while (bd_get_event(m_bd, &m_event))
      ProcessEvent();
  }
}

bool CDVDInputStreamBluray::HasMenu()
{
  return m_navmode;
}

void CDVDInputStreamBluray::SetupPlayerSettings()
{
  int region = CServiceBroker::GetSettings().GetInt(CSettings::SETTING_BLURAY_PLAYERREGION);
  if ( region != BLURAY_REGION_A
    && region != BLURAY_REGION_B
    && region != BLURAY_REGION_C)
  {
    CLog::Log(LOGWARNING, "CDVDInputStreamBluray::Open - Blu-ray region must be set in setting, assuming region A");
    region = BLURAY_REGION_A;
  }
  bd_set_player_setting(m_bd, BLURAY_PLAYER_SETTING_REGION_CODE, region);
  bd_set_player_setting(m_bd, BLURAY_PLAYER_SETTING_PARENTAL, 99);
  bd_set_player_setting(m_bd, BLURAY_PLAYER_SETTING_PLAYER_PROFILE, BLURAY_PLAYER_PROFILE_5_v2_4);
  bd_set_player_setting(m_bd, BLURAY_PLAYER_SETTING_3D_CAP, 0xffffffff);

  std::string langCode;
  g_LangCodeExpander.ConvertToISO6392T(g_langInfo.GetDVDAudioLanguage(), langCode);
  bd_set_player_setting_str(m_bd, BLURAY_PLAYER_SETTING_AUDIO_LANG, langCode.c_str());

  g_LangCodeExpander.ConvertToISO6392T(g_langInfo.GetDVDSubtitleLanguage(), langCode);
  bd_set_player_setting_str(m_bd, BLURAY_PLAYER_SETTING_PG_LANG, langCode.c_str());

  g_LangCodeExpander.ConvertToISO6392T(g_langInfo.GetDVDMenuLanguage(), langCode);
  bd_set_player_setting_str(m_bd, BLURAY_PLAYER_SETTING_MENU_LANG, langCode.c_str());

  g_LangCodeExpander.ConvertToISO6391(g_langInfo.GetRegionLocale(), langCode);
  bd_set_player_setting_str(m_bd, BLURAY_PLAYER_SETTING_COUNTRY_CODE, langCode.c_str());

#ifdef HAVE_LIBBLURAY_BDJ
  std::string cacheDir = CSpecialProtocol::TranslatePath("special://userdata/cache/bluray/cache");
  std::string persistentDir = CSpecialProtocol::TranslatePath("special://userdata/cache/bluray/persistent");
  bd_set_player_setting_str(m_bd, BLURAY_PLAYER_PERSISTENT_ROOT, persistentDir.c_str());
  bd_set_player_setting_str(m_bd, BLURAY_PLAYER_CACHE_ROOT, cacheDir.c_str());
#endif
}

bool CDVDInputStreamBluray::OpenStream(CFileItem &item)
{
  m_pstream.reset(new CDVDInputStreamFile(item));

  if (!m_pstream->Open())
  {
    CLog::Log(LOGERROR, "Error opening image file %s", CURL::GetRedacted(item.GetPath()).c_str());
    Close();
    return false;
  }

  return true;
}
