#include <Windows.h>
#include <Shlwapi.h>
#include <math.h>

#include <stdlib.h>
#include <stdio.h>

static void show_error(const char *msg) {
  fprintf(stderr, "%s\n", msg);
}

int main(int argc, char* argv[]) {

  wchar_t commandline[4096] = L"xfmp4.exe --video_input \\\\.\\pipe\\test_video_pipe --audio_input \\\\.\\pipe\\test_audio_pipe --output test.mp4";


  UINT width = 640;
  UINT height = 480;

  DWORD VIDEO_BUFF_SIZE = 2 * 640 * 480 * sizeof(int);
  DWORD AUDIO_BUFF_SIZE = 44100 / 15 * sizeof(short);
  HANDLE video_pipe = INVALID_HANDLE_VALUE;
  HANDLE audio_pipe = INVALID_HANDLE_VALUE;
  DWORD connect_timeout = 500;

  video_pipe = CreateNamedPipeW(L"\\\\.\\pipe\\test_video_pipe", PIPE_ACCESS_OUTBOUND, PIPE_TYPE_BYTE, PIPE_UNLIMITED_INSTANCES,
    VIDEO_BUFF_SIZE, VIDEO_BUFF_SIZE, connect_timeout, NULL);
  if (video_pipe == INVALID_HANDLE_VALUE) {
    fprintf(stderr, "Failed to create video pipe");
  }

  audio_pipe = CreateNamedPipeW(L"\\\\.\\pipe\\test_audio_pipe", PIPE_ACCESS_OUTBOUND, PIPE_TYPE_BYTE, PIPE_UNLIMITED_INSTANCES,
    AUDIO_BUFF_SIZE, AUDIO_BUFF_SIZE, connect_timeout, NULL);
  if (audio_pipe == INVALID_HANDLE_VALUE) {
    fprintf(stderr, "Failed to create video pipe");
  }

  // create encoding process
  STARTUPINFOW si = {0};
  PROCESS_INFORMATION pi = {0};
  si.cb = sizeof(si);
  if (!CreateProcessW(NULL, commandline, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
    fprintf(stderr, "Failed to create child process");
  }

  // Wait for the client to connect; if it succeeds, 
  // the function returns a nonzero value. If the function
  // returns zero, GetLastError returns ERROR_PIPE_CONNECTED. 
  BOOL video_connected = ConnectNamedPipe(video_pipe, NULL) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED); 
  BOOL audio_connected = ConnectNamedPipe(audio_pipe, NULL) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED); 

  if (!video_connected) {
    CloseHandle(video_pipe);
    video_pipe = INVALID_HANDLE_VALUE;
  }

  if (!audio_connected) {
    CloseHandle(audio_pipe);
    audio_pipe = INVALID_HANDLE_VALUE;
  }

  double time = 0;
  double capture = 0;
  int frame = 0;

  // transfer data
  for (int samples = 0; samples < 44100 * 10; samples += 32) {

    // generate audio data
    short audio_buff[32 * 2];
    for (int i = 0; i < 32; i++) {
      float l = sinf(float(samples + i) / 44.1f) * 32767;
      audio_buff[i * 2 + 0] = (short)l;
      audio_buff[i * 2 + 1] = (short)l;
    }

    DWORD num_written = 0;
    BOOL ret = WriteFile(audio_pipe, audio_buff, sizeof(audio_buff), &num_written, NULL);

    // increase time
    time += 32 / 44100.0;
    if (time > capture) {
      capture += 1.0 / 30.0;
      frame++;

      // generate video data
      UINT size = width * height * 3;
      BYTE *data = new BYTE[size];
      DWORD l = frame * width * 6;
      if (l > size) l = size;
      memset(data, 0, size);
      memset(data, 0xff, l);

      DWORD num_written = 0;
      BOOL ret = WriteFile(video_pipe, data, size, &num_written, NULL);
      delete[] data;

      fprintf(stdout, "frame %d\n", frame);
    }
  }

//finished:
  if (video_pipe != INVALID_HANDLE_VALUE) {
    FlushFileBuffers(video_pipe); 
    DisconnectNamedPipe(video_pipe); 
    CloseHandle(video_pipe); 
  }

  if (audio_pipe != INVALID_HANDLE_VALUE) {
    FlushFileBuffers(audio_pipe); 
    DisconnectNamedPipe(audio_pipe); 
    CloseHandle(audio_pipe); 
  }
  
  // Wait until child process exits.
  WaitForSingleObject(pi.hProcess, INFINITE);

  // Close process and thread handles. 
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);


  return 0;
}