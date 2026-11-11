// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

class QHttpServer;
class Database;

namespace WebApiRoutes {

// db: only used by the opt-in one-bell join (SHADNET_BLOODBORNE_ONE_BELL=1) for the mutual-friend
// check; nullptr = no friend check possible, so the one-bell join stays off.
void RegisterBloodborneRoutes(QHttpServer& http, bool seamlessCoop, Database* db = nullptr);

} // namespace WebApiRoutes
