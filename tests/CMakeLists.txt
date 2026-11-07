# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Huy Luong Duc

add_subdirectory(cli)

# Each module under src/ may only include headers from the modules below
# it. See docs/architecture.md, "Layers".
add_test(NAME layering
	COMMAND sh ${PROJECT_SOURCE_DIR}/scripts/check-layering.sh
		${PROJECT_SOURCE_DIR}/src)
