#define APNAME "Klangbox"

// GPIO PINs
#define BEWEGUNG GPIO_NUM_4
#define VOLUME 34
#define BATTERIE 35

//#define PROTOTYP

#ifdef PROTOTYP
#define BCLK 27;
#define DIN 25;
#define LRC 26;
#else
#define BCLK 25;
#define DIN 32;
#define LRC 27;
#endif

// ULP config
#define ULP_WAKEUP_INTERVAL_MS 100

// Klangbox settings
#define LOOPCNTMAX 45000 // Milliseconds

