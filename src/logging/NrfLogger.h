#pragma once
#include "components/fs/FS.h"
#include "logging/Logger.h"
#include <libraries/memobj/nrf_memobj.h>

#include <FreeRTOS.h>
#include <task.h>

namespace Pinetime {
  namespace Logging {
    class NrfLogger : public Logger {
    public:
      explicit NrfLogger(Controllers::FS* fs);
      ~NrfLogger();

      void Init() override;
      void Resume() override;

      void LogToFile(nrf_memobj_t* p_msg);

    private:
      void WriteLockedMemobjToFile(nrf_memobj_t* p_msg);
      bool EnsureFileOpened();

      static void Process(void*);
      TaskHandle_t m_logger_thread;
      Controllers::FS* m_fs;
      lfs_file_t m_file = {};

      int m_curFileRecordsWritten = 0;
      int m_fileNameCounter = 0;
      static constexpr int kFileNamesToRotate = 10;
      static constexpr int kRecordsToRotate = 50;

      bool m_is_file_opened = false;
      bool m_open_attempt_performed = false;
    };
  }
}
