/*  -*- Mode: c++ -*-
 *   Class ASIRecorder
 *
 *   Copyright (C) Daniel Kristjansson 2010
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 */

// Qt includes
#include <QString>

// MythTV includes
#include "asistreamhandler.h"
#include "asirecorder.h"
#include "asichannel.h"
#include "ringbuffer.h"
#include "tv_rec.h"

#define LOC QString("ASIRec[%1](%2): ") \
            .arg(tvrec ? tvrec->GetInputId() : -1) \
            .arg(m_channel->GetDevice())

ASIRecorder::ASIRecorder(TVRec *rec, ASIChannel *channel) :
    DTVRecorder(rec), m_channel(channel), m_stream_handler(NULL)
{
    SetStreamData(new MPEGStreamData(-1, rec ? rec->GetInputId() : -1,
                                     false));
    if (channel->GetProgramNumber() < 0 || !channel->GetMinorChannel())
        _stream_data->SetListeningDisabled(true);
}

void ASIRecorder::SetOptionsFromProfile(RecordingProfile *profile,
                                        const QString &videodev,
                                        const QString &/*audiodev*/,
                                        const QString &/*vbidev*/)
{
    // We don't want to call DTVRecorder::SetOptionsFromProfile() since
    // we do not have a "recordingtype" in our profile.
    DTVRecorder::SetOption("videodevice", videodev);
    DTVRecorder::SetOption("tvformat", gCoreContext->GetSetting("TVFormat"));
    SetIntOption(profile, "recordmpts");
}

void ASIRecorder::StartNewFile(void)
{
    if (_record_mpts)
        m_stream_handler->AddNamedOutputFile(ringBuffer->GetFilename());

    // Make sure the first things in the file are a PAT & PMT
    HandleSingleProgramPAT(_stream_data->PATSingleProgram(), true);
    HandleSingleProgramPMT(_stream_data->PMTSingleProgram(), true);
}


void ASIRecorder::run(void)
{
    if (!Open())
    {
        _error = "Failed to open device";
        LOG(VB_GENERAL, LOG_ERR, LOC + _error);
        return;
    }

    if (!_stream_data)
    {
        _error = "MPEGStreamData pointer has not been set";
        LOG(VB_GENERAL, LOG_ERR, LOC + _error);
        Close();
        return;
    }

    {
        QMutexLocker locker(&pauseLock);
        request_recording = true;
        recording = true;
        recordingWait.wakeAll();
    }

    if (m_channel->HasGeneratedPAT())
    {
        const ProgramAssociationTable *pat = m_channel->GetGeneratedPAT();
        const ProgramMapTable         *pmt = m_channel->GetGeneratedPMT();
        _stream_data->Reset(pat->ProgramNumber(0));
        _stream_data->HandleTables(MPEG_PAT_PID, *pat);
        _stream_data->HandleTables(pat->ProgramPID(0), *pmt);
    }

    // Listen for time table on DVB standard streams
    if (m_channel && (m_channel->GetSIStandard() == "dvb"))
        _stream_data->AddListeningPID(DVB_TDT_PID);

    StartNewFile();

    _stream_data->AddAVListener(this);
    _stream_data->AddWritingListener(this);
    m_stream_handler->AddListener(
        _stream_data, false, true,
        (_record_mpts) ? ringBuffer->GetFilename() : QString());

    while (IsRecordingRequested() && !IsErrored())
    {
        if (PauseAndWait())
            continue;

        {   // sleep 100 milliseconds unless StopRecording() or Unpause()
            // is called, just to avoid running this too often.
            QMutexLocker locker(&pauseLock);
            if (!request_recording || request_pause)
                continue;
            unpauseWait.wait(&pauseLock, 100);
        }

        if (!_input_pmt)
        {
            LOG(VB_GENERAL, LOG_WARNING, LOC +
                "Recording will not commence until a PMT is set.");
            usleep(5000);
            continue;
        }

        if (!m_stream_handler->IsRunning())
        {
            _error = "Stream handler died unexpectedly.";
            LOG(VB_GENERAL, LOG_ERR, LOC + _error);
        }
    }

    m_stream_handler->RemoveListener(_stream_data);
    _stream_data->RemoveWritingListener(this);
    _stream_data->RemoveAVListener(this);

    Close();

    FinishRecording();

    QMutexLocker locker(&pauseLock);
    recording = false;
    recordingWait.wakeAll();
}

bool ASIRecorder::Open(void)
{
    if (IsOpen())
    {
        LOG(VB_GENERAL, LOG_WARNING, LOC + "Card already open");
        return true;
    }

    ResetForNewFile();

    m_stream_handler = ASIStreamHandler::Get(m_channel->GetDevice(),
                                             tvrec ? tvrec->GetInputId() : -1);


    LOG(VB_RECORD, LOG_INFO, LOC + "Opened successfully");

    return true;
}

bool ASIRecorder::IsOpen(void) const
{
    return m_stream_handler;
}

void ASIRecorder::Close(void)
{
    LOG(VB_RECORD, LOG_INFO, LOC + "Close() -- begin");

    if (IsOpen())
        ASIStreamHandler::Return(m_stream_handler,
                                 tvrec ? tvrec->GetInputId() : -1);

    LOG(VB_RECORD, LOG_INFO, LOC + "Close() -- end");
}
