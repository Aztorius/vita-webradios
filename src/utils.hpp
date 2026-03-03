#ifndef __UTILS_HPP__
#define __UTILS_HPP__

int copyfile(const char *destfile, const char *srcfile);
void Utils_InitPowerTick(void);
void Utils_LockPower(void);
void Utils_UnlockPower(void);

#endif
