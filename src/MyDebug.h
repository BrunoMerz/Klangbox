/**
   MyDebug.h
   Class for debugging purposes.

   @autor    Bruno Merz
   @version  1.1
   @created  30.12.2019
   @updated  19.03.2021

*/

//#define myDEBUG   // generall switch in order to turn on debugging for all classes

#if defined(myDEBUG)
#define DEBUG_PRINT(x) Serial.print(x)
#define DEBUG_PRINT2(x, y) Serial.print(x, y)
#define DEBUG_PRINTLN(x) Serial.println(x)
#define DEBUG_PRINTLN2(x, y) Serial.println(x, y)
#define DEBUG_PRINTF   Serial.printf
#define DEBUG_FLUSH() Serial.flush()
#define DEBUG_BEGIN(x) Serial.begin(x)
#define DEBUG_SERIAL   Serial
#else
#define DEBUG_PRINT(x)
#define DEBUG_PRINT2(x, y)
#define DEBUG_PRINTLN(x)
#define DEBUG_PRINTLN2(x, y)
#define DEBUG_PRINTF
#define DEBUG_FLUSH()
#define DEBUG_BEGIN(x)
#define DEBUG_SERIAL   1
#endif
