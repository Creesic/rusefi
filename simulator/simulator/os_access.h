/*
 * os_access.h
 *
 * OS access is not part of global.h in order to help separate synchronous code from asynchronous
 *
 *  Created on: Jul 3, 2019
 * @author Andrey Belomutskiy, (c) 2012-2020
 */

#ifndef OS_ACCESS_H_
#define OS_ACCESS_H_

#ifdef __cplusplus
extern "C"
{
#endif /* __cplusplus */

#include <ch.h>
#include <hal.h>

#ifdef __cplusplus
}
#endif /* __cplusplus */

#define HAS_OS_ACCESS

#endif /* OS_ACCESS_H_ */
