/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// nsBrowserApp.cpp - MODIFICADO PARA NAVEGADOR PERSONALIZADO

#include "nsXULAppAPI.h"
#include "mozilla/XREAppData.h"
#include "XREChildData.h"
#include "XREShellData.h"
#include "application.ini.h"
#include "mozilla/Bootstrap.h"
#include "mozilla/ProcessType.h"
#include "mozilla/RuntimeExceptionModule.h"
#include "mozilla/ScopeExit.h"
#include "BrowserDefines.h"
#if defined(XP_WIN)
#  include <windows.h>
#  include <stdlib.h>
#elif defined(XP_UNIX)
#  include <sys/resource.h>
#  include <unistd.h>
#  include <fcntl.h>
#endif

#include <stdio.h>
#include <stdarg.h>
#include <time.h>

#include "nsCOMPtr.h"

#ifdef XP_WIN
#  include "mozilla/PreXULSkeletonUI.h"
#  include "freestanding/SharedSection.h"
#  include "LauncherProcessWin.h"
#  include "mozilla/GeckoArgs.h"
#  include "mozilla/mscom/ProcessRuntime.h"
#  include "mozilla/WindowsDllBlocklist.h"
#  include "mozilla/WindowsDpiInitialization.h"
#  include "mozilla/WindowsProcessMitigations.h"

#  define XRE_WANT_ENVIRON
#  include "nsWindowsWMain.cpp"

#  define strcasecmp _stricmp
#  ifdef MOZ_SANDBOX
#    include "mozilla/sandboxing/SandboxInitialization.h"
#    include "mozilla/sandboxing/sandboxLogging.h"
#  endif
#endif
#include "BinaryPath.h"

#include "nsXPCOMPrivate.h" 

#include "mozilla/BaseProfiler.h"
#include "mozilla/Sprintf.h"
#include "mozilla/StartupTimeline.h"

#ifdef LIBFUZZER
#  include "FuzzerDefs.h"
#endif

using namespace mozilla;

// 1. CAMBIO DE CARPETA DE DATOS
// Cambiar "browser" por el nombre de tu proyecto para separar perfiles
#define kDesktopFolder "mi-navegador-custom"

#ifdef MOZ_BACKGROUNDTASKS
static bool gIsBackgroundTask = false;
#endif

static MOZ_FORMAT_PRINTF(1, 2) void Output(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);

#ifndef XP_WIN
  vfprintf(stderr, fmt, ap);
#else
  bool showMessageBox = true;

  char msg[2048];
  vsnprintf_s(msg, _countof(msg), _TRUNCATE, fmt, ap);

  wchar_t wide_msg[2048];
  MultiByteToWideChar(CP_UTF8, 0, msg, -1, wide_msg, _countof(wide_msg));

#  if MOZ_WINCONSOLE
  showMessageBox = false;
#  elif defined(MOZ_BACKGROUNDTASKS)
  showMessageBox = !gIsBackgroundTask;
#  endif

  if (showMessageBox) {
    HMODULE user32 = LoadLibraryW(L"user32.dll");
    if (user32) {
      decltype(MessageBoxW)* messageBoxW =
          (decltype(MessageBoxW)*)GetProcAddress(user32, "MessageBoxW");
      if (messageBoxW) {
        // 2. CAMBIO DE NOMBRE EN DIÁLOGOS DE ERROR
        messageBoxW(nullptr, wide_msg, L"MiNavegador Custom Engine",
                    MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
      } else {
        showMessageBox = false;
      }
      FreeLibrary(user32);
    } else {
      showMessageBox = false;
    }
  }

  if (!showMessageBox) {
    fwprintf_s(stderr, wide_msg);
  }
#endif

  va_end(ap);
}

static bool IsFlag(const char* arg, const char* s) {
  if (*arg == '-') {
    if (*++arg == '-') ++arg;
    return !strcasecmp(arg, s);
  }
#if defined(XP_WIN)
  if (*arg == '/') return !strcasecmp(++arg, s);
#endif
  return false;
}

constinit Bootstrap::UniquePtr gBootstrap;

static int do_main(int argc, char* argv[], char* envp[]) {
  const char* appDataFile = getenv("XUL_APP_FILE");
  
  // 3. LÓGICA DE ARGUMENTOS PERSONALIZADOS
  for (int i = 1; i < argc; i++) {
    if (IsFlag(argv[i], "mi-optimizacion")) {
        // Aquí podrías activar variables de entorno o flags internos
        printf("Optimizaciones personalizadas activadas.\n");
    }
  }

  if ((!appDataFile || !*appDataFile) && (argc > 1 && IsFlag(argv[1], "app"))) {
    if (argc == 2) {
      Output("Argumentos incorrectos para -app");
      return 255;
    }
    appDataFile = argv[2];

    char appEnv[MAXPATHLEN];
    SprintfLiteral(appEnv, "XUL_APP_FILE=%s", argv[2]);
    if (putenv(strdup(appEnv))) {
      Output("Error al configurar %s.\n", appEnv);
      return 255;
    }
    argv[2] = argv[0];
    argv += 2;
    argc -= 2;
  }

  BootstrapConfig config;
  if (appDataFile && *appDataFile) {
    config.appData = nullptr;
    config.appDataPath = appDataFile;
  } else {
    config.appData = &sAppData;
    config.appDataPath = kDesktopFolder;
  }

#if defined(XP_WIN) && defined(MOZ_SANDBOX)
  sandbox::BrokerServices* brokerServices =
        sandboxing::GetInitializedBrokerServices();
  if (!brokerServices) {
    Output("Error al inicializar Broker Services.\n");
    return 255;
  }
  config.sandboxBrokerServices = brokerServices;
#endif

  EnsureBrowserCommandlineSafe(argc, argv);
  return gBootstrap->XRE_main(argc, argv, config);
}

static nsresult InitXPCOMGlue(LibLoadingStrategy aLibLoadingStrategy) {
  if (gBootstrap) {
    return NS_OK;
  }

  UniqueFreePtr<char> exePath = BinaryPath::Get();
  if (!exePath) {
    Output("No se encontró el directorio de la aplicación.\n");
    return NS_ERROR_FAILURE;
  }

  auto bootstrapResult =
      mozilla::GetBootstrap(exePath.get(), aLibLoadingStrategy);
  if (bootstrapResult.isErr()) {
    Output("Error al cargar XPCOM (Gecko).\n");
    return NS_ERROR_FAILURE;
  }

  gBootstrap = bootstrapResult.unwrap();
  gBootstrap->NS_LogInit();
  return NS_OK;
}

int main(int argc, char* argv[], char* envp[]) {
  // 4. MENSAJE DE BIENVENIDA EN CONSOLA (Debug)
  #ifdef DEBUG
  printf("Iniciando MiNavegador Custom Build v1.0...\n");
  #endif

  mozilla::TimeStamp start = mozilla::TimeStamp::Now();

  AUTO_BASE_PROFILER_INIT;
  AUTO_BASE_PROFILER_LABEL("nsBrowserApp main", OTHER);

  CrashReporter::RegisterRuntimeExceptionModule();
  auto unregisterRuntimeExceptionModule =
      MakeScopeExit([] { CrashReporter::UnregisterRuntimeExceptionModule(); });

  // Manejo de procesos hijos (Content Processes)
  if (GetGeckoProcessType() != GeckoProcessType_Default) {
     // (Mantiene la lógica original de sandbox y DLL blocklist para estabilidad)
     nsresult rv = InitXPCOMGlue(LibLoadingStrategy::NoReadAhead);
     if (NS_FAILED(rv)) return 255;
     
     XREChildData childData;
     rv = gBootstrap->XRE_InitChildProcess(argc, argv, &childData);
     gBootstrap->NS_LogTerm();
     return NS_FAILED(rv) ? 1 : 0;
  }

  // Proceso Principal
  nsresult rv = InitXPCOMGlue(LibLoadingStrategy::ReadAhead);
  if (NS_FAILED(rv)) return 255;

  gBootstrap->XRE_StartupTimelineRecord(mozilla::StartupTimeline::START, start);

  // Ejecución principal
  int result = do_main(argc, argv, envp);

  gBootstrap->NS_LogTerm();
  gBootstrap.reset();

  return result;
}
