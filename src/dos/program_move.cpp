// SPDX-FileCopyrightText:  2023-2025 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

#include "program_move.h"

#include <string>

#include "shell.h"
#include "string_utils.h"

void MOVE::Run()
{
	std::string tmp = "";
	cmd->GetStringRemain(tmp);

	char args[CMD_MAXLINE];
	safe_strcpy(args, tmp.c_str());

	auto shell = DOS_GetFirstShell();
	assert(shell);

	shell->CMD_MOVE(args);
}
