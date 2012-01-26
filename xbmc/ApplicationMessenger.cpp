/*
 *      Copyright (C) 2005-2008 Team XBMC
 *      http://www.xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */

#include "system.h"
#include "ApplicationMessenger.h"
#include "Application.h"

#include "TextureManager.h"
#include "PlayListPlayer.h"
#include "Util.h"
#include "SectionLoader.h"
#ifdef HAS_PYTHON
#include "lib/libPython/XBPython.h"
#endif
#include "GUIWindowSlideShow.h"
#ifdef HAS_WEB_SERVER
#include "lib/libGoAhead/XBMChttp.h"
#endif
#include "utils/Builtins.h"
#include "utils/Network.h"
#include "GUIWindowManager.h"
#include "GUIWindowManager.h"
#include "Settings.h"
#include "GUISettings.h"
#include "FileItem.h"
#include "GUIDialog.h"
#include "WindowingFactory.h"

#include "PowerManager.h"

#ifdef _WIN32
#include "WIN32Util.h"
#define CHalManager CWIN32Util
#elif defined __APPLE__
#include "CocoaInterface.h"
#endif
#include "MediaManager.h"
#include "LocalizeStrings.h"
#include "SingleLock.h"

#include "AppManager.h"

#include "BackgroundInfoLoader.h"
#include "LocalizeStrings.h"
#include "GUIDialogProgress.h"
#include "GUIDialogOK2.h"


#include "utils/log.h"
#include "LangInfo.h"

#include "VideoRenderers/RenderManager.h"

#include "GUIDialogBoxeePostPlay.h"

using namespace std;

CApplicationMessenger::CApplicationMessenger()
{
  m_bgProcessor.SetName("Application Messenger");
  m_bgProcessor.Start(1);
}

CApplicationMessenger::~CApplicationMessenger()
{
  Cleanup();
  m_bgProcessor.SignalStop();
  m_bgProcessor.Stop();
}

// Boxee
bool CApplicationMessenger::InitializeWindow(CGUIWindow *pWindow)
{
  ThreadMessage tMsg = {TMSG_INIT_WINDOW};
  tMsg.lpVoid = pWindow;
  SendMessage(tMsg, true);  
  return tMsg.dwParam1 != 0;
}

bool CApplicationMessenger::RunJob(BOXEE::BXBGJob *aJob)
{
  return m_bgProcessor.QueueJob(aJob);
}
// end Boxee

void CApplicationMessenger::Cleanup()
{
  while (m_vecMessages.size() > 0)
  {
    ThreadMessage* pMsg = m_vecMessages.front();
    delete pMsg;
    m_vecMessages.pop_front();
  }

  while (m_vecWindowMessages.size() > 0)
  {
    ThreadMessage* pMsg = m_vecWindowMessages.front();
    delete pMsg;
    m_vecWindowMessages.pop_front();
  }
}

void CApplicationMessenger::ReleaseThreadMessages(ThreadIdentifier thread)
{
  if (thread == 0)
    return;
  
  CSingleLock lock (m_critSection);
  std::deque<ThreadMessage*>::iterator i = m_vecMessages.begin();
  while (i != m_vecMessages.end())
  {
    ThreadMessage* msg = *i;
    if (msg && msg->waitingThreadId && CThread::Equals(msg->waitingThreadId,thread) && msg->hWaitEvent)
    {
      ::SetEvent(msg->hWaitEvent);
      i = m_vecMessages.erase(i);
      delete msg;
    }
    else
      i++;
  }
}

void CApplicationMessenger::SendMessage(ThreadMessage& message, bool wait)
{
  message.waitingThreadId = CThread::GetCurrentThreadId();
  message.hWaitEvent = NULL;
  if (wait)
  { // check that we're not being called from our application thread, else we'll be waiting
    // forever!
    if (!g_application.IsCurrentThread())
      message.hWaitEvent = CreateEvent(NULL, true, false, NULL);
    else
    {
      //OutputDebugString("Attempting to wait on a SendMessage() from our application thread will cause lockup!\n");
      //OutputDebugString("Sending immediately\n");
      ProcessMessage(&message);
      return;
    }
  }

  ThreadMessage* msg = new ThreadMessage();
  msg->dwMessage = message.dwMessage;
  msg->dwParam1 = message.dwParam1;
  msg->dwParam2 = message.dwParam2;
  msg->hWaitEvent = message.hWaitEvent;
  msg->lpVoid = message.lpVoid;
  msg->strParam = message.strParam;
  msg->strParam2 = message.strParam2;
  msg->params = message.params;
  msg->waitingThreadId = message.waitingThreadId;

  CSingleLock lock (m_critSection);
  if (m_bSwallowMessages == false)
  {
    if (msg->dwMessage == TMSG_DIALOG_DOMODAL ||
        msg->dwMessage == TMSG_DIALOG_PROGRESS_SHOWMODAL ||
        msg->dwMessage == TMSG_WRITE_SCRIPT_OUTPUT)
      m_vecWindowMessages.push_back(msg);
    else 
      m_vecMessages.push_back(msg);
  } 
  else 
  {
    if (message.hWaitEvent)
      CloseHandle(message.hWaitEvent);
    delete msg;
    message.hWaitEvent = NULL;
  }
  lock.Leave();

  if (message.hWaitEvent)
  {
    while(m_bSwallowMessages == false)
    {
      if(WAIT_OBJECT_0 == WaitForSingleObject(message.hWaitEvent, 500))
        break;     
    }

    CloseHandle(message.hWaitEvent);
    message.hWaitEvent = NULL;
  }
}

void CApplicationMessenger::ProcessMessages()
{
  // process threadmessages
  CSingleLock lock (m_critSection);
  while (m_vecMessages.size() > 0)
  {
    ThreadMessage* pMsg = m_vecMessages.front();
    //first remove the message from the queue, else the message could be processed more then once
    m_vecMessages.pop_front();

    //Leave here as the message might make another
    //thread call processmessages or sendmessage
    lock.Leave();

    ProcessMessage(pMsg);
    if (pMsg->hWaitEvent)
      SetEvent(pMsg->hWaitEvent);
    delete pMsg;

    lock.Enter();
  }
}

void CApplicationMessenger::ProcessMessage(ThreadMessage *pMsg)
{
  switch (pMsg->dwMessage)
  {
    case TMSG_SHUTDOWN:
      {
        switch (g_guiSettings.GetInt("system.shutdownstate"))
        {
          case POWERSTATE_SHUTDOWN:
            Powerdown();
            break;

          case POWERSTATE_SUSPEND:
            Suspend();
            break;

          case POWERSTATE_HIBERNATE:
            Hibernate();
            break;

          case POWERSTATE_QUIT:
            Quit();
            break;

          case POWERSTATE_MINIMIZE:
            Minimize();
            break;
          }
        }
      break;

case TMSG_POWERDOWN:
      {
        g_application.Stop();
        Sleep(200);
        g_Windowing.DestroyWindow();
        g_powerManager.Powerdown();
          exit(64);
        }
      break;

    case TMSG_QUIT:
      {
        g_application.Stop();
        Sleep(200);
        exit(0);
      }
      break;

    case TMSG_HIBERNATE:
      {
        g_powerManager.Hibernate();
      }
      break;

    case TMSG_SUSPEND:
      {
        g_powerManager.Suspend();
      }
      break;
    case TMSG_LOGOUT:
      {
        CLog::Log(LOGDEBUG,"CApplicationMessenger::ProcessMessage - Enter TMSG_LOGOUT case. Going to call BoxeeLoginManager::Logout() (logout)");
        g_application.GetBoxeeLoginManager().Logout();
      }
      break;
    case TMSG_RESTART:
      {
        g_application.Stop();
        Sleep(200);
        g_Windowing.DestroyWindow();
        g_powerManager.Reboot();
        exit(66);
      }
      break;

    case TMSG_RESET:
      {
        g_application.Stop();
        Sleep(200);
        g_Windowing.DestroyWindow();
        g_powerManager.Reboot();
        exit(66);
      }
      break;

    case TMSG_RESTARTAPP:
      {
#ifdef _WIN32
        g_application.Stop();
        Sleep(200);
#endif
        exit(65);
        // TODO
        //char szXBEFileName[1024];

        //CIoSupport::GetXbePath(szXBEFileName);
        //CUtil::RunXBE(szXBEFileName);
      }
      break;

    case TMSG_MEDIA_PLAY:
      {
        // first check if we were called from the PlayFile() function
        if (pMsg->lpVoid && pMsg->dwParam2 == 0)
        {
          CFileItem *item = (CFileItem *)pMsg->lpVoid;
          g_application.PlayFile(*item, pMsg->dwParam1 != 0);
          delete item;
          return;
        }

        // restore to previous window if needed
        if (g_windowManager.GetActiveWindow() == WINDOW_SLIDESHOW ||
            g_windowManager.GetActiveWindow() == WINDOW_FULLSCREEN_VIDEO ||
            g_windowManager.GetActiveWindow() == WINDOW_VISUALISATION)
          g_windowManager.PreviousWindow();

        g_application.ResetScreenSaver();
        g_application.WakeUpScreenSaverAndDPMS();

        //g_application.StopPlaying();
        // play file
        CFileItem *item;
        if (pMsg->lpVoid && pMsg->dwParam2 == 1)
        {
          item = (CFileItem *)pMsg->lpVoid;
        }
        else
        {
          item = new CFileItem(pMsg->strParam, false);
        }
        
        if (item->IsAudio())
          item->SetMusicThumb();
        else
          item->SetVideoThumb();
        item->FillInDefaultIcon();
        g_application.PlayMedia(*item, item->IsAudio() ? PLAYLIST_MUSIC : PLAYLIST_VIDEO); //Note: this will play playlists always in the temp music playlist (default 2nd parameter), maybe needs some tweaking.
        delete item;
      }
      break;

    case TMSG_MEDIA_RESTART:
      g_application.Restart(true);
      break;

    case TMSG_PICTURE_SHOW:
      {
        CGUIWindowSlideShow *pSlideShow = (CGUIWindowSlideShow *)g_windowManager.GetWindow(WINDOW_SLIDESHOW);
        if (!pSlideShow) return ;

        // stop playing file
        if (g_application.IsPlayingVideo()) g_application.StopPlaying();

        if (g_windowManager.GetActiveWindow() == WINDOW_FULLSCREEN_VIDEO)
          g_windowManager.PreviousWindow();

        g_application.ResetScreenSaver();
        g_application.WakeUpScreenSaverAndDPMS();

        g_graphicsContext.Lock();
        pSlideShow->Reset();
        if (g_windowManager.GetActiveWindow() != WINDOW_SLIDESHOW)
          g_windowManager.ActivateWindow(WINDOW_SLIDESHOW);
        if (CUtil::IsZIP(pMsg->strParam) || CUtil::IsRAR(pMsg->strParam)) // actually a cbz/cbr
        {
          CFileItemList items;
          CStdString strPath;
          if (CUtil::IsZIP(pMsg->strParam))
            CUtil::CreateArchivePath(strPath, "zip", pMsg->strParam.c_str(), "");
          else
            CUtil::CreateArchivePath(strPath, "rar", pMsg->strParam.c_str(), "");

          CUtil::GetRecursiveListing(strPath, items, g_stSettings.m_pictureExtensions);
          if (items.Size() > 0)
          {
            for (int i=0;i<items.Size();++i)
            {
              pSlideShow->Add(items[i].get());
            }
            pSlideShow->Select(items[0]->m_strPath);
          }
        }
        else
        {
          CFileItem item(pMsg->strParam, false);
          pSlideShow->Add(&item);
          pSlideShow->Select(pMsg->strParam);
        }
        g_graphicsContext.Unlock();
      }
      break;

    case TMSG_SLIDESHOW_SCREENSAVER:
    case TMSG_PICTURE_SLIDESHOW:
      {
        CGUIWindowSlideShow *pSlideShow = (CGUIWindowSlideShow *)g_windowManager.GetWindow(WINDOW_SLIDESHOW);
        if (!pSlideShow) return ;

        g_graphicsContext.Lock();
        pSlideShow->Reset();

        CFileItemList items;
        CStdString strPath = pMsg->strParam;
        if (pMsg->dwMessage == TMSG_SLIDESHOW_SCREENSAVER &&
            g_guiSettings.GetString("screensaver.mode").Equals("Fanart Slideshow"))
        {
          CUtil::GetRecursiveListing(g_settings.GetVideoFanartFolder(), items, ".tbn");
          CUtil::GetRecursiveListing(g_settings.GetMusicFanartFolder(), items, ".tbn");
        }
        else
        CUtil::GetRecursiveListing(strPath, items, g_stSettings.m_pictureExtensions);

        if (items.Size() > 0)
        {
          for (int i=0;i<items.Size();++i)
            pSlideShow->Add(items[i].get());
          pSlideShow->StartSlideShow(pMsg->dwMessage == TMSG_SLIDESHOW_SCREENSAVER); //Start the slideshow!
        }
        if (pMsg->dwMessage == TMSG_SLIDESHOW_SCREENSAVER)
          pSlideShow->Shuffle();

        if (g_windowManager.GetActiveWindow() != WINDOW_SLIDESHOW)
          g_windowManager.ActivateWindow(WINDOW_SLIDESHOW);

        g_graphicsContext.Unlock();
      }
      break;

    case TMSG_MEDIA_STOP:
      {
        // restore to previous window if needed
        if (g_windowManager.GetActiveWindow() == WINDOW_SLIDESHOW ||
            g_windowManager.GetActiveWindow() == WINDOW_FULLSCREEN_VIDEO ||
            g_windowManager.GetActiveWindow() == WINDOW_VISUALISATION)
          g_windowManager.PreviousWindow();

        g_application.ResetScreenSaver();
        g_application.WakeUpScreenSaverAndDPMS();

        // stop playing file
        if (g_application.IsPlaying()) g_application.StopPlaying();
      }
      break;

    case TMSG_MEDIA_PAUSE:
      if (g_application.m_pPlayer)
      {
        g_application.ResetScreenSaver();
        g_application.WakeUpScreenSaverAndDPMS();
        g_application.m_pPlayer->Pause();
      }
      break;

    case TMSG_SWITCHTOFULLSCREEN:
      if( g_windowManager.GetActiveWindow() != WINDOW_FULLSCREEN_VIDEO && g_windowManager.GetActiveWindow() != WINDOW_KARAOKELYRICS)
        g_application.SwitchToFullScreen();
      break;

    case TMSG_MINIMIZE:
      g_application.Minimize();
      break;

    case TMSG_EXECUTE_OS:
#if defined( _LINUX) && !defined(__APPLE__)
      CUtil::RunCommandLine(pMsg->strParam.c_str(), (pMsg->dwParam1 == 1));
#elif defined(_WIN32)
      CWIN32Util::XBMCShellExecute(pMsg->strParam.c_str(), (pMsg->dwParam1 == 1));
#endif
      break;

    case TMSG_HTTPAPI:
    {
#ifdef HAS_WEB_SERVER
      if (!m_pXbmcHttp)
      {
        CSectionLoader::Load("LIBHTTP");
        m_pXbmcHttp = new CXbmcHttp();
      }
      switch (m_pXbmcHttp->xbmcCommand(pMsg->strParam))
      {
      case 1:
        g_application.getApplicationMessenger().Restart();
        break;

      case 2:
        g_application.getApplicationMessenger().Shutdown();
        break;

      case 3:
        g_application.getApplicationMessenger().RebootToDashBoard();
        break;

      case 4:
        g_application.getApplicationMessenger().Reset();
        break;

      case 5:
        g_application.getApplicationMessenger().RestartApp();
        break;
      }
#endif
    }
     break;

    case TMSG_EXECUTE_SCRIPT:
#ifdef HAS_PYTHON
      g_pythonParser.evalFile(pMsg->strParam.c_str());
#endif
      break;

    case TMSG_EXECUTE_BUILT_IN:
      CBuiltins::Execute(pMsg->strParam.c_str());
      break;

    case TMSG_PLAYLISTPLAYER_PLAY:
      if (pMsg->dwParam1 != (DWORD) -1)
        g_playlistPlayer.Play(pMsg->dwParam1);
      else
        g_playlistPlayer.Play();

      break;

    case TMSG_PLAYLISTPLAYER_NEXT:
      g_playlistPlayer.PlayNext();
      break;

    case TMSG_PLAYLISTPLAYER_PREV:
      g_playlistPlayer.PlayPrevious();
      break;

    // Window messages below here...
    case TMSG_DIALOG_DOMODAL:  //doModel of window
      {
        CGUIDialog* pDialog = (CGUIDialog*)g_windowManager.GetWindow(pMsg->dwParam1);
        if (!pDialog) return ;
        pDialog->DoModal();
      }
      break;

    case TMSG_DIALOG_PROGRESS_SHOWMODAL:
      {
        CGUIDialogProgress* pDialog = (CGUIDialogProgress*)g_windowManager.GetWindow(WINDOW_DIALOG_PROGRESS);
        if (!pDialog) return ;
        pDialog->StartModal();
      }
      break;
      
    case TMSG_WRITE_SCRIPT_OUTPUT:
      {
        //send message to window 2004 (CGUIWindowScriptsInfo)
        CGUIMessage msg(GUI_MSG_USER, 0, 0);
        msg.SetLabel(pMsg->strParam);
        CGUIWindow* pWindowScripts = g_windowManager.GetWindow(WINDOW_SCRIPTS_INFO);
        if (pWindowScripts) pWindowScripts->OnMessage(msg);
      }
      break;

    case TMSG_NETWORKMESSAGE:
      {
        g_application.getNetwork().NetworkMessage((CNetwork::EMESSAGE)pMsg->dwParam1, (int)pMsg->dwParam2);
      }
      break;

    case TMSG_GUI_DO_MODAL:
      {
        CGUIDialog *pDialog = (CGUIDialog *)pMsg->lpVoid;
        if (pDialog)
          pDialog->DoModal_Internal((int)pMsg->dwParam1, pMsg->strParam);
      }
      break;

    case TMSG_GUI_SHOW:
      {
        CGUIDialog *pDialog = (CGUIDialog *)pMsg->lpVoid;
        if (pDialog)
          pDialog->Show_Internal();
      }
      break;
    case TMSG_LOAD_STRINGS:
	  g_localizeStrings.Load(pMsg->strParam, pMsg->strParam2);
          break;
    case TMSG_GUI_ACTIVATE_WINDOW:
      {
        g_windowManager.ActivateWindow(pMsg->dwParam1, pMsg->params, pMsg->dwParam2 > 0);
      }
      break;

    case TMSG_GUI_WIN_MANAGER_PROCESS:
      g_windowManager.Process_Internal(0 != pMsg->dwParam1);
      break;

    case TMSG_GUI_WIN_MANAGER_RENDER:
      g_windowManager.Render_Internal();
      break;

#ifdef HAS_DVD_DRIVE
    case TMSG_OPTICAL_MOUNT:
      {
        CMediaSource share;
        share.strStatus = g_mediaManager.GetDiskLabel(share.strPath);
        share.strPath = pMsg->strParam;
        if(g_mediaManager.IsAudio(share.strPath))
          share.strStatus = "Audio-CD";
        else if(share.strStatus == "")
          share.strStatus = g_localizeStrings.Get(446);
        share.strName = share.strPath;
        share.m_ignore = true;
        share.m_iDriveType = CMediaSource::SOURCE_TYPE_DVD;
        g_mediaManager.AddAutoSource(share, pMsg->dwParam1 != 0);
      }
      break;

    case TMSG_OPTICAL_UNMOUNT:
      {
        CMediaSource share;
        share.strPath = pMsg->strParam;
        share.strName = share.strPath;
        g_mediaManager.RemoveAutoSource(share);
      }
      break;
#endif
      //Boxee
    case TMSG_INIT_WINDOW:
    {
      CGUIWindow *pWindow = (CGUIWindow *)pMsg->lpVoid;
      if (pWindow)
        pMsg->dwParam1 = pWindow->Initialize();
    }
      break;
    case TMSG_LOAD_LANG_INFO:
    {
      pMsg->dwParam1 = g_langInfo.Load(pMsg->strParam);
      break; 

    }
      break;
    case TMSG_DELETE_BG_LOADER:
    {
      CBackgroundInfoLoader *pLoader = (CBackgroundInfoLoader *)pMsg->lpVoid;
      if (pLoader)
        delete pLoader;
    }
      break;
    case TMSG_GENERAL_MESSAGE:
    {
      CGUIMessage *msg = (CGUIMessage *)pMsg->lpVoid;
      if (msg)
      {
        CGUIWindow* pWindow = g_windowManager.GetWindow(pMsg->dwParam1);
        if (pWindow) 
          pWindow->OnMessage(*msg);
        
        if (!pMsg->hWaitEvent) // no one waits for this message to return.
          delete msg;
      }
    }
      break;
    case TMSG_SET_CONTROL_LABEL:
    {
      CGUIWindow* pWindow = g_windowManager.GetWindow(pMsg->dwParam1);
      if (pWindow) 
      {
        CGUIControl *control = (CGUIControl *)pWindow->GetControl(pMsg->dwParam2);
        if (control)
        {
          CGUIMessage msg(GUI_MSG_LABEL_SET, pMsg->dwParam1, pMsg->dwParam2);
          msg.SetLabel(pMsg->strParam);
          control->OnMessage(msg);
        }
      }
    }
    break;
            
    case TMSG_CLOSE_DIALOG:
    {
      CGUIDialog *dlg = (CGUIDialog *)pMsg->lpVoid;
      bool bForce = (bool)pMsg->dwParam1;
      if (dlg)
        dlg->Close(bForce);
    }
    break;
    
    case TMSG_PREVIOUS_WINDOW:
    {
      g_windowManager.PreviousWindow();
    }
    break;
    
    case TMSG_TOGGLEFULLSCREEN:
    {
      CAction action;
      action.id = ACTION_TOGGLE_FULLSCREEN;
      g_application.OnAction(action);

    }
    break; 

    case TMSG_FREE_WINDOW_RESOURCES:
    {
      CGUIWindow *win = (CGUIWindow *)pMsg->lpVoid;
      if (win)
        win->FreeResources();
    }
    break; 

    case TMSG_FREE_TEXTURE:
    {
      CGUITextureBase *t = (CGUITextureBase *)pMsg->lpVoid;
      if (t)
        t->FreeResources(!!pMsg->dwParam1);
    }
    break; 
      
    case TMSG_VIDEO_RENDERER_PREINIT:
    {
      pMsg->dwParam1 = g_renderManager.PreInit();
      break; 
    }
    
    case TMSG_VIDEO_RENDERER_UNINIT:
    {
      g_renderManager.UnInit();
      break; 
    }

    case TMSG_CLOSE_SLIDESHOWPIC:
    {
      CSlideShowPic *t = (CSlideShowPic *)pMsg->lpVoid;
      if (t)
        t->Close();
      break; 
    }
      
    case TMSG_SHOW_POST_PLAY_DIALOG:
    {
      CFileItem *item = (CFileItem *)pMsg->lpVoid;
      CGUIDialogBoxeePostPlay *dlg = (CGUIDialogBoxeePostPlay *)g_windowManager.GetWindow(WINDOW_DIALOG_BOXEE_POST_PLAY);
      dlg->SetItem(item);
      dlg->DoModal();
      dlg->Reset(); 
      delete item;
      break; 
    }

    case TMSG_SHOW_PLAY_ERROR:
    {
      CGUIDialogOK2::ShowAndGetInput(g_localizeStrings.Get(257), pMsg->strParam);
      break; 
    }

    case TMSG_EXECUTE_ON_MAIN_THREAD:
    {
      IGUIThreadTask *t = (IGUIThreadTask *)(pMsg->lpVoid);
      if (t)
      {
        t->DoWork();
        if (pMsg->dwParam1 == 1)
          delete t;
      }
      break; 
    }
      
    case TMSG_DELETE_PLAYER:
    {
      IPlayer *player = (IPlayer *)pMsg->lpVoid;
      printf("deleting player %p\n", player);
      if (player)
        delete player;
    }
      break;
      
      //end Boxee
  }
}

void CApplicationMessenger::ProcessWindowMessages()
{
  CSingleLock lock (m_critSection);
  //message type is window, process window messages
  while (m_vecWindowMessages.size() > 0)
  {
    ThreadMessage* pMsg = m_vecWindowMessages.front();
      //first remove the message from the queue, else the message could be processed more then once
    m_vecWindowMessages.pop_front();

      // leave here in case we make more thread messages from this one
      lock.Leave();

      ProcessMessage(pMsg);
      if (pMsg->hWaitEvent)
        SetEvent(pMsg->hWaitEvent);
      delete pMsg;

      lock.Enter();
    }
  }

int CApplicationMessenger::SetResponse(CStdString response)
{
  CSingleLock lock (m_critBuffer);
  bufferResponse=response;
  lock.Leave();
  return 0;
}

CStdString CApplicationMessenger::GetResponse()
{
  CStdString tmp;
  CSingleLock lock (m_critBuffer);
  tmp=bufferResponse;
  lock.Leave();
  return tmp;
}

void CApplicationMessenger::HttpApi(string cmd, bool wait)
{
  SetResponse("");
  ThreadMessage tMsg = {TMSG_HTTPAPI};
  tMsg.strParam = cmd;
  SendMessage(tMsg, wait);
}

void CApplicationMessenger::ExecBuiltIn(const CStdString &command)
{
  ThreadMessage tMsg = {TMSG_EXECUTE_BUILT_IN};
  tMsg.strParam = command;
  SendMessage(tMsg, true);
}

void CApplicationMessenger::MediaPlay(string filename)
{
  ThreadMessage tMsg = {TMSG_MEDIA_PLAY};
  tMsg.strParam = filename;
  SendMessage(tMsg, true);
}

void CApplicationMessenger::MediaPlay(const CFileItem &item)
{
  ThreadMessage tMsg = {TMSG_MEDIA_PLAY};
  CFileItem *pItem = new CFileItem(item);
  tMsg.lpVoid = (void *)pItem;
  tMsg.dwParam2 = 1;
  SendMessage(tMsg, false);
}

void CApplicationMessenger::PlayFile(const CFileItem &item, bool bRestart /*= false*/)
{
  ThreadMessage tMsg = {TMSG_MEDIA_PLAY};
  CFileItem *pItem = new CFileItem(item);
  tMsg.lpVoid = (void *)pItem;
  tMsg.dwParam1 = bRestart ? 1 : 0;
  tMsg.dwParam2 = 0;
  SendMessage(tMsg, false);
}

void CApplicationMessenger::MediaStop()
{
  ThreadMessage tMsg = {TMSG_MEDIA_STOP};
  SendMessage(tMsg, true);
}

void CApplicationMessenger::MediaPause()
{
  ThreadMessage tMsg = {TMSG_MEDIA_PAUSE};
  SendMessage(tMsg, true);
}

void CApplicationMessenger::MediaRestart(bool bWait)
{
  ThreadMessage tMsg = {TMSG_MEDIA_RESTART};
  SendMessage(tMsg, bWait);
}

void CApplicationMessenger::PlayListPlayerPlay()
{
  ThreadMessage tMsg = {TMSG_PLAYLISTPLAYER_PLAY, (DWORD) -1};
  SendMessage(tMsg, true);
}

void CApplicationMessenger::PlayListPlayerPlay(int iSong)
{
  ThreadMessage tMsg = {TMSG_PLAYLISTPLAYER_PLAY, iSong};
  SendMessage(tMsg, true);
}

void CApplicationMessenger::PlayListPlayerNext()
{
  ThreadMessage tMsg = {TMSG_PLAYLISTPLAYER_NEXT};
  SendMessage(tMsg, true);
}

void CApplicationMessenger::PlayListPlayerPrevious()
{
  ThreadMessage tMsg = {TMSG_PLAYLISTPLAYER_PREV};
  SendMessage(tMsg, true);
}

void CApplicationMessenger::PictureShow(string filename)
{
  ThreadMessage tMsg = {TMSG_PICTURE_SHOW};
  tMsg.strParam = filename;
  SendMessage(tMsg);
}

void CApplicationMessenger::PictureSlideShow(string pathname, bool bScreensaver /* = false */)
{
  DWORD dwMessage = TMSG_PICTURE_SLIDESHOW;
  if (bScreensaver)
    dwMessage = TMSG_SLIDESHOW_SCREENSAVER;
  ThreadMessage tMsg = {dwMessage};
  tMsg.strParam = pathname;
  SendMessage(tMsg);
}

void CApplicationMessenger::Shutdown()
{
  ThreadMessage tMsg = {TMSG_SHUTDOWN};
  SendMessage(tMsg);
}

void CApplicationMessenger::Powerdown()
{
  ThreadMessage tMsg = {TMSG_POWERDOWN};
  SendMessage(tMsg);
}

void CApplicationMessenger::Quit()
{
  ThreadMessage tMsg = {TMSG_QUIT};
  SendMessage(tMsg);
}

void CApplicationMessenger::Hibernate()
{
  ThreadMessage tMsg = {TMSG_HIBERNATE};
  SendMessage(tMsg);
}

void CApplicationMessenger::Suspend()
{
  ThreadMessage tMsg = {TMSG_SUSPEND};
  SendMessage(tMsg);
}

void CApplicationMessenger::Logout()
{
  ThreadMessage tMsg = {TMSG_LOGOUT};
  SendMessage(tMsg);
}

void CApplicationMessenger::Restart()
{
  ThreadMessage tMsg = {TMSG_RESTART};
  SendMessage(tMsg);
}

void CApplicationMessenger::Reset()
{
  ThreadMessage tMsg = {TMSG_RESET};
  SendMessage(tMsg);
}

void CApplicationMessenger::RestartApp()
{
  ThreadMessage tMsg = {TMSG_RESTARTAPP};
  SendMessage(tMsg);
}

void CApplicationMessenger::RebootToDashBoard()
{
  ThreadMessage tMsg = {TMSG_DASHBOARD};
  SendMessage(tMsg);
}

void CApplicationMessenger::NetworkMessage(DWORD dwMessage, DWORD dwParam)
{
  ThreadMessage tMsg = {TMSG_NETWORKMESSAGE, dwMessage, dwParam};
  SendMessage(tMsg);
}

void CApplicationMessenger::SwitchToFullscreen()
{
  /* FIXME: ideally this call should return upon a successfull switch but currently
     is causing deadlocks between the dvdplayer destructor and the rendermanager
  */
  ThreadMessage tMsg = {TMSG_SWITCHTOFULLSCREEN};
  SendMessage(tMsg, false);
}

void CApplicationMessenger::ToggleFullscreen()
{
  ThreadMessage tMsg = {TMSG_TOGGLEFULLSCREEN};
  SendMessage(tMsg, false);
}

void CApplicationMessenger::LoadStrings(const CStdString& strFileName, const CStdString& strFallbackFileName)
{
  ThreadMessage tMsg = {TMSG_LOAD_STRINGS};
  tMsg.strParam = strFileName;
  tMsg.strParam2 = strFallbackFileName;
  SendMessage(tMsg, true);
}

void CApplicationMessenger::Minimize(bool wait)
{
  ThreadMessage tMsg = {TMSG_MINIMIZE};
  SendMessage(tMsg, wait);
}

void CApplicationMessenger::DoModal(CGUIDialog *pDialog, int iWindowID, const CStdString &param)
{
  ThreadMessage tMsg = {TMSG_GUI_DO_MODAL};
  tMsg.lpVoid = pDialog;
  tMsg.dwParam1 = (DWORD)iWindowID;
  tMsg.strParam = param;
  SendMessage(tMsg, true);
}

void CApplicationMessenger::DialogProgressShow()
{
  ThreadMessage tMsg = {TMSG_DIALOG_PROGRESS_SHOWMODAL};
  SendMessage(tMsg, true);
}

void CApplicationMessenger::ExecOS(const CStdString command, bool waitExit)
{
  ThreadMessage tMsg = {TMSG_EXECUTE_OS};
  tMsg.strParam = command;
  tMsg.dwParam1 = (DWORD)waitExit;
  SendMessage(tMsg, false);
}

void CApplicationMessenger::Show(CGUIDialog *pDialog)
{
  ThreadMessage tMsg = {TMSG_GUI_SHOW};
  tMsg.lpVoid = pDialog;
  SendMessage(tMsg, true);
}

void CApplicationMessenger::ActivateWindow(int windowID, const vector<CStdString> &params, bool swappingWindows)
{
  ThreadMessage tMsg = {TMSG_GUI_ACTIVATE_WINDOW, windowID, swappingWindows ? 1 : 0};
  tMsg.params = params;
  SendMessage(tMsg, true);
}

void CApplicationMessenger::WindowManagerProcess(bool renderOnly)
{
  ThreadMessage tMsg = {TMSG_GUI_WIN_MANAGER_PROCESS};
  tMsg.dwParam1 = (DWORD)renderOnly;
  SendMessage(tMsg, true);
}

void CApplicationMessenger::Render()
{
  ThreadMessage tMsg = {TMSG_GUI_WIN_MANAGER_RENDER};
  SendMessage(tMsg, true);
}

void CApplicationMessenger::OpticalMount(CStdString device, bool bautorun) 
{
  ThreadMessage tMsg = {TMSG_OPTICAL_MOUNT};
  tMsg.strParam = device;
  tMsg.dwParam1 = (DWORD)bautorun;
  SendMessage(tMsg, false);
}
 
void CApplicationMessenger::OpticalUnMount(CStdString device)
{
  ThreadMessage tMsg = {TMSG_OPTICAL_UNMOUNT};
  tMsg.strParam = device;
  SendMessage(tMsg, false);
}

bool CApplicationMessenger::LoadLangInfo(const CStdString &strFileName)
{
  ThreadMessage tMsg = {TMSG_LOAD_LANG_INFO};
  tMsg.strParam = strFileName;
  SendMessage(tMsg, true);  
  return tMsg.dwParam1 != 0;
}

void CApplicationMessenger::CloseDialog(CGUIDialog *dlg, bool bForce)
{
  ThreadMessage tMsg = {TMSG_CLOSE_DIALOG};
  tMsg.lpVoid = dlg;
  tMsg.dwParam1 = bForce;
  SendMessage(tMsg, true);  
}

void CApplicationMessenger::SetLabel(CGUIControl* control, int iWindowID, const CStdString &label)
{
  std::vector<CStdString> vec;
  ThreadMessage tMsg = { TMSG_SET_CONTROL_LABEL, iWindowID, control->GetID(), label, vec, "", NULL, control };
  SendMessage(tMsg, true); 
}

void CApplicationMessenger::SendGUIMessageToWindow(CGUIMessage& message, int iWindowId, bool wait)
{
  if (g_application.IsCurrentThread() && wait)
  {
    g_windowManager.SendMessage(message);
  }
  else 
  {
    ThreadMessage tMsg = {TMSG_GENERAL_MESSAGE};
    tMsg.dwParam1 = iWindowId;
    if (wait)
    {
      tMsg.lpVoid = (void*) &message; 
    }
    else
    {
      // allocate a new gui message as it will be deleted after the message is done
      tMsg.lpVoid = new CGUIMessage(message);
    }
    
    SendMessage(tMsg, wait);
  }
}

void CApplicationMessenger::PreviousWindow()
{
  ThreadMessage tMsg = {TMSG_PREVIOUS_WINDOW};
  SendMessage(tMsg, true);  
}

void CApplicationMessenger::WindowFreeResources(CGUIWindow *win)
{
  ThreadMessage tMsg = {TMSG_FREE_WINDOW_RESOURCES};
  tMsg.lpVoid = win;
  SendMessage(tMsg, true);  
}

void CApplicationMessenger::ExecuteOnMainThread(IGUIThreadTask *t, bool wait, bool autoDel)
{
  ThreadMessage tMsg = {TMSG_EXECUTE_ON_MAIN_THREAD};
  tMsg.lpVoid = t;
  tMsg.dwParam1 = (autoDel?1:0);
  SendMessage(tMsg, wait);  
}

void CApplicationMessenger::SetSwallowMessages(bool bSwallow)
{
  CSingleLock lock(m_critSection);
  m_bSwallowMessages = bSwallow;
}