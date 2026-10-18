// Единственная единица трансляции с реализацией miniaudio. Набор отключаемых модулей
// (MA_NO_*) задаётся в CMake для всех файлов библиотеки сразу -- он меняет размеры структур.
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>
