# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2023 Huawei Device Co., Ltd.
#
obj-$(CONFIG_SECURITY_CODE_SIGN) += \
	code_sign_misc.o \
	verify_cert_chain.o \
	code_sign_ioctl.o \
	code_sign_ext.o

ccflags-$(CONFIG_SECURITY_CODE_SIGN) += \
	-I$(srctree)/fs/verity \
	-I$(srctree)/fs/code_sign
