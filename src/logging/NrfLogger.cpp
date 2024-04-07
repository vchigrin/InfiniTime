#include "logging/NrfLogger.h"

#include <algorithm>
#include <libraries/log/nrf_log.h>
#include <libraries/log/nrf_log_ctrl.h>
#include <libraries/log/nrf_log_default_backends.h>
#include <libraries/memobj/nrf_memobj.h>

#ifdef PINETIME_IS_RECOVERY_LOADER
#define FLASH_LOG_ENABLED 0
#else
#define FLASH_LOG_ENABLED NRF_LOG_ENABLED
#endif


using namespace Pinetime::Logging;

namespace {

#if FLASH_LOG_ENABLED

void nrf_log_backend_fs_panic_set(nrf_log_backend_t const * p_backend);
void nrf_log_backend_fs_flush(nrf_log_backend_t const * p_backend);
void nrf_log_backend_fs_put(nrf_log_backend_t const * p_backend,
                            nrf_log_entry_t * p_msg);

const nrf_log_backend_api_t nrf_log_backend_fs_api = {
 .put       = nrf_log_backend_fs_put,
 .panic_set = nrf_log_backend_fs_panic_set,
 .flush     = nrf_log_backend_fs_flush,
};

struct NrfUserContext {
  NrfLogger* logger;
};

NrfUserContext g_user_context {
  .logger = nullptr
};

//NRF_LOG_BACKEND_DEF(fs_log_backend, nrf_log_backend_fs_api, &g_user_context);

static nrf_log_backend_cb_t log_backend_cb_fs_log_backend = {
  .p_next  = NULL,
  .id      = NRF_LOG_BACKEND_INVALID_ID,
  .enabled = false,
};

NRF_SECTION_ITEM_REGISTER(NRF_LOG_BACKEND_SUBSECTION_NAME(fs_log_backend),
      static const nrf_log_backend_t fs_log_backend) = {
        .p_api = &nrf_log_backend_fs_api,
        .p_ctx = &g_user_context,
        .p_name = const_cast<char*>("fs_log_backend"),
        .p_cb = &log_backend_cb_fs_log_backend,
};

void AddFsBackend() {
  int32_t backend_id = nrf_log_backend_add(&fs_log_backend, NRF_LOG_SEVERITY_DEBUG);
  ASSERT(backend_id >= 0);
  nrf_log_backend_enable(&fs_log_backend);
}

void nrf_log_backend_fs_panic_set(nrf_log_backend_t const * p_backend) {
 // auto* context = reinterpret_cast<NrfUserContext*>(p_backend->p_ctx);
 // // Panic happened - stop passing any data to logging backend.
 // context->logger = nullptr;
 (void)p_backend;
}

void nrf_log_backend_fs_flush(nrf_log_backend_t const * p_backend) {
  (void)p_backend;
}

void nrf_log_backend_fs_put(nrf_log_backend_t const * p_backend,
                            nrf_log_entry_t * p_msg) {
  auto* context = reinterpret_cast<NrfUserContext*>(p_backend->p_ctx);
  if (context->logger) {
    context->logger->LogToFile(p_msg);
  }
}

constexpr size_t kBufSize = 64;
uint8_t g_msg_buf[kBufSize];

#endif
}  // namespace

NrfLogger::NrfLogger(Controllers::FS* fs)
    : m_fs(fs) {
#if FLASH_LOG_ENABLED
  // Only one instance of this class must be present in system.
  g_user_context.logger = this;
#endif
}

NrfLogger::~NrfLogger() {
#if FLASH_LOG_ENABLED
  g_user_context.logger = nullptr;
#endif
}

void NrfLogger::Init() {
  auto result = NRF_LOG_INIT(nullptr);
  APP_ERROR_CHECK(result);

  NRF_LOG_DEFAULT_BACKENDS_INIT();

  if (pdPASS != xTaskCreate(NrfLogger::Process, "LOGGER", 200, this, 0, &m_logger_thread)) {
    APP_ERROR_HANDLER(NRF_ERROR_NO_MEM);
  }
#if FLASH_LOG_ENABLED
  AddFsBackend();
#endif
}

void NrfLogger::Process(void*) {
  NRF_LOG_INFO("Logger task started!");

#pragma clang diagnostic push
#pragma ide diagnostic ignored "EndlessLoop"
  while (true) {
    NRF_LOG_FLUSH();
    vTaskDelay(100); // Not good for power consumption, it will wake up every 100ms...
  }
#pragma clang diagnostic pop
}

void NrfLogger::Resume() {
  vTaskResume(m_logger_thread);
}

#if FLASH_LOG_ENABLED
void NrfLogger::LogToFile(nrf_log_entry_t * p_msg) {
  if (!EnsureFileOpened()) {
    return;
  }
  nrf_memobj_get(p_msg);
  WriteLockedMemobjToFile(p_msg);
  nrf_memobj_put(p_msg);
}

void NrfLogger::WriteLockedMemobjToFile(nrf_log_entry_t * p_msg) {
  nrf_log_header_t header = {};
  const size_t memobj_offset = HEADER_SIZE*sizeof(uint32_t);
  nrf_memobj_read(p_msg, &header, HEADER_SIZE*sizeof(uint32_t), 0);
  size_t data_len = 0;

  m_fs->FileWrite(
      &m_file,
      reinterpret_cast<const uint8_t*>(&header),
      sizeof(header));

  switch (header.base.generic.type) {
    case HEADER_TYPE_STD: {
      data_len = header.base.std.nargs * sizeof(uint32_t);
      break;
    }
    case HEADER_TYPE_HEXDUMP: {
      data_len = header.base.hexdump.len;
      break;
    }
    default:
      data_len = 0;
  }
  if (data_len != 0) {
    data_len = std::min(data_len, kBufSize);
    nrf_memobj_read(p_msg, g_msg_buf, data_len, memobj_offset);
    m_fs->FileWrite(&m_file, g_msg_buf, data_len);
  }
  m_curFileRecordsWritten++;
}

bool NrfLogger::EnsureFileOpened() {
  if (!m_fs) {
    return false;
  }
  if (!m_fs->isInitialized()) {
    return false;
  }
  if (m_is_file_opened && m_curFileRecordsWritten >= kRecordsToRotate) {
    m_fs->FileClose(&m_file);
    m_is_file_opened = false;
    m_open_attempt_performed = false;
    m_curFileRecordsWritten = 0;
    m_fileNameCounter = (m_fileNameCounter + 1) % kFileNamesToRotate;
  }
  if (m_is_file_opened || m_open_attempt_performed) {
    return true;
  }
  char file_name_buf[20];
  sprintf(file_name_buf, "/debug_%d.log", m_fileNameCounter);
  const int result = m_fs->FileOpen(
      &m_file,
      file_name_buf,
      LFS_O_WRONLY | LFS_O_CREAT);
  m_open_attempt_performed = true;
  if (result == LFS_ERR_OK) {
    m_is_file_opened = true;
    return true;
  } else {
    return false;
  }

}
#endif
