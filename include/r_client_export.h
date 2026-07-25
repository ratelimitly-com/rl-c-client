#ifndef R_CLIENT_EXPORT_H
#define R_CLIENT_EXPORT_H

#if defined(_WIN32) && defined(RCLIENT_SHARED)
#if defined(RCLIENT_BUILDING_DLL)
#define RCLIENT_API __declspec(dllexport)
#else
#define RCLIENT_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define RCLIENT_API __attribute__((visibility("default")))
#else
#define RCLIENT_API
#endif

#endif
