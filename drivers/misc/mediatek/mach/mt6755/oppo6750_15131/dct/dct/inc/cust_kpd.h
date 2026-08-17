#ifndef __CUST_KPD_H__
#define __CUST_KPD_H__
#define KPD_KCOL0_PIN (107|0x80000000)
#define KPD_KCOL1_PIN (108|0x80000000)
#define KPD_KROW0_PIN (110|0x80000000)
#define KPD_KROW1_PIN (111|0x80000000)
#define KPD_PMIC_RSTKEY_MAP KEY_POWER
#define KPD_PWRKEY_MAP KEY_POWER
#define KPD_USE_EXTEND_TYPE 0
/* Keypad matrix is 2x2 but no keys mapped (vol keys use GPIO EINT, power uses PMIC) */
#define KPD_INIT_KEYMAP() { \
 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, \
 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, \
 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, \
 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, \
 0, 0, 0, 0, 0, 0, 0, 0 \
}
#endif
