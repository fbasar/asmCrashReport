// References:
//    https://spin.atomicobject.com/2013/01/13/exceptions-stack-traces-c/
//    http://theorangeduck.com/page/printing-stack-trace-mingw

// In order for this to work w/mingw:
//    - you need to link with Dbghelp.dll
//    - you need to turn on debug symbols with "-g"

// In order for this to work w/macOS:
//    - you need to turn on C/CXX debug symbols with "-g"
//    - you need to turn off C/CXX "Position Independent Executable" with "-fno-pie"
//    - you need to turn off linker "Position Independent Executable" with "-Wl,-no_pie"
//    - it helps to turn off optimization with "-O0"
//    - it helps to never omit frame pointers with "-fno-omit-frame-pointer"
//
// Andy Maloney <asmaloney@gmail.com>

#include <stdlib.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QProcess>
#include <QStandardPaths>
#include <QStringList>
#include <QTextStream>

#ifdef Q_OS_WIN
#include <windows.h>
#include <imagehlp.h>
#else
#include <err.h>
#include <execinfo.h>
#include <signal.h>
#endif

#include "asmCrashReport.h"


namespace asmCrashReport
{
   static QString sProgramName;        // the full path to the executable (which we need to resolve symbols)
   static QString sCrashReportDirPath; // where to store the crash reports

   static logWrittenCallback  sLogWrittenCallback; // function to call after we've written the log file

   // Resolve symbol name & source location
   QString _addr2line( const QString &inProgramName, int inFrameNumber, void const * const inAddr )
   {
      QString  command;

      // map the address to the line in the code
#ifdef Q_OS_MAC
      command = QStringLiteral( "atos" );

      command += QStringLiteral( " -o \"%1\"" ).arg( inProgramName );
      command += QStringLiteral( " -arch x86_64" );
      command += QStringLiteral( " 0x%1" ).arg( quintptr( inAddr ), 0, 16 );
#else
      command = QStringLiteral( "%1/tools/addr2line" ).arg( QCoreApplication::applicationDirPath() );

      command += QStringLiteral( " -f -p" );
      command += QStringLiteral( " -e %1" ).arg( inProgramName );
      command += QStringLiteral( " 0x%1" ).arg( quintptr( inAddr ), 0, 16 );
#endif

      QProcess *myProcess = new QProcess;

      myProcess->setProcessChannelMode( QProcess::MergedChannels );
      myProcess->start( command, QIODevice::ReadOnly );

      QString  frameInfo;

      if ( !myProcess->waitForFinished() )
      {
         frameInfo = QStringLiteral( "* Error running command\n   %1\n   %2" ).arg( command, myProcess->errorString() );
      }
      else
      {
         frameInfo = QStringLiteral( "[%1] %2" ).arg( QString::number( inFrameNumber ), myProcess->readAll().constData() );
      }

      delete myProcess;

      return frameInfo.trimmed();
   }

   void  _writeLog( const QString &inSignal, const QStringList &inFrameInfoList )
   {
      const QStringList cReportHeader{
         QStringLiteral( "%1 v%2" ).arg( QCoreApplication::applicationName(), QCoreApplication::applicationVersion() ),
               QDateTime::currentDateTime().toString( "dd MMM yyyy @ HH:mm:ss" ),
               QString(),
               inSignal,
               QString(),
      };

      QDir().mkpath( sCrashReportDirPath );

      const QString  cFileName = QStringLiteral( "%1/%2 %3 Crash.log" ).arg( sCrashReportDirPath,
                                                                             QDateTime::currentDateTime().toString( "yyyyMMdd-HHmmss" ),
                                                                             QCoreApplication::applicationName() );

      QFile file( cFileName );

      if ( file.open( QIODevice::WriteOnly | QIODevice::Text ) )
      {
         QTextStream stream( &file );

         for ( const QString &data : cReportHeader + inFrameInfoList )
         {
            stream << data << endl;
         }
      }

      if ( sLogWrittenCallback != nullptr )
      {
         (*sLogWrittenCallback)( cFileName );
      }
   }

#ifdef Q_OS_WIN
   QStringList _stackTrace( CONTEXT* context )
   {
      QStringList frameList;

      HANDLE process = GetCurrentProcess();
      HANDLE thread = GetCurrentThread();

      SymInitialize( process, 0, true );

      STACKFRAME64 stackFrame;
      memset( &stackFrame, 0, sizeof( STACKFRAME64 ) );

      DWORD   image;

#ifdef _M_IX86
      image = IMAGE_FILE_MACHINE_I386;

      stackFrame.AddrPC.Offset = context->Eip;
      stackFrame.AddrPC.Mode = AddrModeFlat;
      stackFrame.AddrStack.Offset = context->Esp;
      stackFrame.AddrStack.Mode = AddrModeFlat;
      stackFrame.AddrFrame.Offset = context->Ebp;
      stackFrame.AddrFrame.Mode = AddrModeFlat;
#else
#error You need to define the stack frame layout for this architecture
#endif

      int   frameNumber = 0;

      while ( StackWalk64(
                 image, process, thread,
                 &stackFrame, context, NULL,
                 SymFunctionTableAccess64, SymGetModuleBase64, NULL )
              )
      {
         frameList += _addr2line( sProgramName, frameNumber, (void*)stackFrame.AddrPC.Offset );

         ++frameNumber;
      }

      SymCleanup( GetCurrentProcess() );

      return frameList;
   }

   LONG WINAPI _winExceptionHandler( EXCEPTION_POINTERS *inExceptionInfo )
   {
      const QString  cExceptionType = [] ( DWORD code ) {
         switch( code )
         {
            case EXCEPTION_ACCESS_VIOLATION:
               return QStringLiteral( "EXCEPTION_ACCESS_VIOLATION" );
            case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
               return QStringLiteral( "EXCEPTION_ARRAY_BOUNDS_EXCEEDED" );
            case EXCEPTION_BREAKPOINT:
               return QStringLiteral( "EXCEPTION_BREAKPOINT" );
            case EXCEPTION_DATATYPE_MISALIGNMENT:
               return QStringLiteral( "EXCEPTION_DATATYPE_MISALIGNMENT" );
            case EXCEPTION_FLT_DENORMAL_OPERAND:
               return QStringLiteral( "EXCEPTION_FLT_DENORMAL_OPERAND" );
            case EXCEPTION_FLT_DIVIDE_BY_ZERO:
               return QStringLiteral( "EXCEPTION_FLT_DIVIDE_BY_ZERO" );
            case EXCEPTION_FLT_INEXACT_RESULT:
               return QStringLiteral( "EXCEPTION_FLT_INEXACT_RESULT" );
            case EXCEPTION_FLT_INVALID_OPERATION:
               return QStringLiteral( "EXCEPTION_FLT_INVALID_OPERATION" );
            case EXCEPTION_FLT_OVERFLOW:
               return QStringLiteral( "EXCEPTION_FLT_OVERFLOW" );
            case EXCEPTION_FLT_STACK_CHECK:
               return QStringLiteral( "EXCEPTION_FLT_STACK_CHECK" );
            case EXCEPTION_FLT_UNDERFLOW:
               return QStringLiteral( "EXCEPTION_FLT_UNDERFLOW" );
            case EXCEPTION_ILLEGAL_INSTRUCTION:
               return QStringLiteral( "EXCEPTION_ILLEGAL_INSTRUCTION" );
            case EXCEPTION_IN_PAGE_ERROR:
               return QStringLiteral( "EXCEPTION_IN_PAGE_ERROR" );
            case EXCEPTION_INT_DIVIDE_BY_ZERO:
               return QStringLiteral( "EXCEPTION_INT_DIVIDE_BY_ZERO" );
            case EXCEPTION_INT_OVERFLOW:
               return QStringLiteral( "EXCEPTION_INT_OVERFLOW" );
            case EXCEPTION_INVALID_DISPOSITION:
               return QStringLiteral( "EXCEPTION_INVALID_DISPOSITION" );
            case EXCEPTION_NONCONTINUABLE_EXCEPTION:
               return QStringLiteral( "EXCEPTION_NONCONTINUABLE_EXCEPTION" );
            case EXCEPTION_PRIV_INSTRUCTION:
               return QStringLiteral( "EXCEPTION_PRIV_INSTRUCTION" );
            case EXCEPTION_SINGLE_STEP:
               return QStringLiteral( "EXCEPTION_SINGLE_STEP" );
            case EXCEPTION_STACK_OVERFLOW:
               return QStringLiteral( "EXCEPTION_STACK_OVERFLOW" );
            default:
               return QStringLiteral( "Unrecognized Exception" );
         }
      } ( inExceptionInfo->ExceptionRecord->ExceptionCode );

      // If this is a stack overflow then we can't walk the stack, so just show
      // where the error happened
      QStringList frameInfoList;

      if ( inExceptionInfo->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW )
      {
         frameInfoList += _addr2line( sProgramName, 0, (void*)inExceptionInfo->ContextRecord->Eip );
      }
      else
      {
         frameInfoList = _stackTrace( inExceptionInfo->ContextRecord );
      }

      _writeLog( cExceptionType, frameInfoList );

      return EXCEPTION_EXECUTE_HANDLER;
   }
#else
   QStringList _stackTrace()
   {
      constexpr int MAX_STACK_FRAMES = 64;
      void          *sStackTraces[MAX_STACK_FRAMES];

      int   traceSize = backtrace( sStackTraces, MAX_STACK_FRAMES );
      char  **messages = backtrace_symbols( sStackTraces, traceSize );

      // skip the first 2 stack frames (this function and our handler) and skip the last frame (always junk)
      QStringList frameList;
      int         frameNumber = 0;

      for ( int i = 3; i < (traceSize - 1); ++i )
      {
         frameList += _addr2line( sProgramName, frameNumber, sStackTraces[i] );

         ++frameNumber;
      }

      if ( messages != nullptr )
      {
         free( messages );
      }

      return frameList;
   }

   // prtotype to prevent warning about not returning
   void _posixSignalHandler( int inSig, siginfo_t *inSigInfo, void *inContext ) __attribute__ ((noreturn));
   void _posixSignalHandler( int inSig, siginfo_t *inSigInfo, void *inContext )
   {
      Q_UNUSED( inContext );

      const QString  cSignalType = [] ( int sig, int inSignalCode ) {
         switch( sig )
         {
            case SIGSEGV:
               return QStringLiteral( "Caught SIGSEGV: Segmentation Fault" );
            case SIGINT:
               return QStringLiteral( "Caught SIGINT: Interactive attention signal, (usually ctrl+c)" );
            case SIGFPE:
               switch( inSignalCode )
               {
                  case FPE_INTDIV:
                     return QStringLiteral( "Caught SIGFPE: (integer divide by zero)" );
                  case FPE_INTOVF:
                     return QStringLiteral( "Caught SIGFPE: (integer overflow)" );
                  case FPE_FLTDIV:
                     return QStringLiteral( "Caught SIGFPE: (floating-point divide by zero)" );
                  case FPE_FLTOVF:
                     return QStringLiteral( "Caught SIGFPE: (floating-point overflow)" );
                  case FPE_FLTUND:
                     return QStringLiteral( "Caught SIGFPE: (floating-point underflow)" );
                  case FPE_FLTRES:
                     return QStringLiteral( "Caught SIGFPE: (floating-point inexact result)" );
                  case FPE_FLTINV:
                     return QStringLiteral( "Caught SIGFPE: (floating-point invalid operation)" );
                  case FPE_FLTSUB:
                     return QStringLiteral( "Caught SIGFPE: (subscript out of range)" );
                  default:
                     return QStringLiteral( "Caught SIGFPE: Arithmetic Exception" );
               }
            case SIGILL:
               switch( inSignalCode )
               {
                  case ILL_ILLOPC:
                     return QStringLiteral( "Caught SIGILL: (illegal opcode)" );
                  case ILL_ILLOPN:
                     return QStringLiteral( "Caught SIGILL: (illegal operand)" );
                  case ILL_ILLADR:
                     return QStringLiteral( "Caught SIGILL: (illegal addressing mode)" );
                  case ILL_ILLTRP:
                     return QStringLiteral( "Caught SIGILL: (illegal trap)" );
                  case ILL_PRVOPC:
                     return QStringLiteral( "Caught SIGILL: (privileged opcode)" );
                  case ILL_PRVREG:
                     return QStringLiteral( "Caught SIGILL: (privileged register)" );
                  case ILL_COPROC:
                     return QStringLiteral( "Caught SIGILL: (coprocessor error)" );
                  case ILL_BADSTK:
                     return QStringLiteral( "Caught SIGILL: (internal stack error)" );
                  default:
                     return QStringLiteral( "Caught SIGILL: Illegal Instruction" );
               }
            case SIGTERM:
               return QStringLiteral( "Caught SIGTERM: a termination request was sent to the program" );
            case SIGABRT:
               return QStringLiteral( "Caught SIGABRT: usually caused by an abort() or assert()" );
         }

         return QStringLiteral( "Unrecognized Signal" );
      } ( inSig, inSigInfo->si_code );

      const QStringList cFrameInfoList = _stackTrace();

      _writeLog( cSignalType, cFrameInfoList );

      _Exit(1);
   }

   void _posixSetupSignalHandler()
   {
      // setup alternate stack
      static uint8_t alternateStack[SIGSTKSZ];

      stack_t ss{ static_cast<void*>(alternateStack), SIGSTKSZ, 0 };

      if ( sigaltstack( &ss, nullptr ) != 0 )
      {
         err( 1, "sigaltstack" );
      }

      // register our signal handlers
      struct sigaction sigAction;

      sigAction.sa_sigaction = _posixSignalHandler;

      sigemptyset( &sigAction.sa_mask );

#ifdef __APPLE__
      // for some reason backtrace() doesn't work on macOS when we use an alternate stack
      sigAction.sa_flags = SA_SIGINFO;
#else
      sigAction.sa_flags = SA_SIGINFO | SA_ONSTACK;
#endif

      if ( sigaction( SIGSEGV, &sigAction, nullptr ) != 0 ) { err( 1, "sigaction" ); }
      if ( sigaction( SIGFPE,  &sigAction, nullptr ) != 0 ) { err( 1, "sigaction" ); }
      if ( sigaction( SIGINT,  &sigAction, nullptr ) != 0 ) { err( 1, "sigaction" ); }
      if ( sigaction( SIGILL,  &sigAction, nullptr ) != 0 ) { err( 1, "sigaction" ); }
      if ( sigaction( SIGTERM, &sigAction, nullptr ) != 0 ) { err( 1, "sigaction" ); }
      if ( sigaction( SIGABRT, &sigAction, nullptr ) != 0 ) { err( 1, "sigaction" ); }
   }
#endif

   void  setSignalHandler( const QString &inCrashReportDirPath, logWrittenCallback inLogWrittenCallback )
   {
      sProgramName = QCoreApplication::arguments().at( 0 );

      sCrashReportDirPath = inCrashReportDirPath;

      if ( sCrashReportDirPath.isEmpty() )
      {
         sCrashReportDirPath = QStringLiteral( "%1/%2 Crash Logs" ).arg( QStandardPaths::writableLocation( QStandardPaths::DesktopLocation ),
                                                                         QCoreApplication::applicationName() );
      }

      sLogWrittenCallback = inLogWrittenCallback;

#ifdef Q_OS_WIN
      SetUnhandledExceptionFilter( _winExceptionHandler );
#else
      _posixSetupSignalHandler();
#endif
   }
}