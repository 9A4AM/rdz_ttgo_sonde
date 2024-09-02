#ifndef _LOGGER_H_
#define _LOGGER_H_

#include <stdarg.h>
#include "Sonde.h"

enum LOGLEVEL { L_ERR, L_WARN, L_INFO, L_DBG };

#define LOG_E(...) Log.logf(L_ERR, __VA_ARGS__)
#define LOG_W(...) Log.logf(L_WARN, __VA_ARGS__)
#define LOG_I(...) Log.logf(L_INFO, __VA_ARGS__)
#define LOG_D(...) Log.logf(L_DBG, __VA_ARGS__)

class Logger {
   public: 
   void logf(LOGLEVEL lvl, const char *module, const char *fmt, ...);
};


extern Logger Log;

#endif
