#pragma once

// Shared Client/Station release: a.b. All releases within major a interoperate.
// Increment a for incompatible Client/Station behavior; b for compatible changes.
// Independent of LoRa wire and NVS record versions. Debug is a build label.
#define SHORE_SPOTTER_RELEASE "1.0"
#ifdef FIELD_DIAGNOSTIC
#define SHORE_SPOTTER_VERSION SHORE_SPOTTER_RELEASE "-debug"
#else
#define SHORE_SPOTTER_VERSION SHORE_SPOTTER_RELEASE
#endif
