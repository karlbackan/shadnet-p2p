// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "webapi_routes_bloodborne.h"

#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <optional>

#include <QDateTime>
#include <QDebug>
#include <QHttpHeaders>
#include <QHttpServer>
#include <QHttpServerRequest>
#include <QHttpServerResponse>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QList>
#include <QString>

#include "bloodborne_summon_broker.h"
#include "database.h"

namespace WebApiRoutes {
namespace {

constexpr auto HostPlacementHeader = "X-ShadPS4-Bloodborne-Host-Placement";

QHttpServerResponse SummonResponse(const QString& messageId, const QByteArray& hostPlacement = {}) {
    QJsonObject body;
    body.insert(QStringLiteral("ResKind"), 0);
    body.insert(QStringLiteral("MessageId"), messageId);
    QHttpServerResponse response{"application/json",
                                 QJsonDocument(body).toJson(QJsonDocument::Compact),
                                 QHttpServerResponse::StatusCode::Ok};
    if (!hostPlacement.isEmpty()) {
        QHttpHeaders headers = response.headers();
        if (headers.append(HostPlacementHeader, hostPlacement)) {
            response.setHeaders(std::move(headers));
        }
    }
    return response;
}

QHttpServerResponse SummonListResponse(const QList<QByteArray>& signs) {
    QByteArray body = "{\"SummonDataList\":[";
    for (qsizetype index = 0; index < signs.size(); ++index) {
        if (index != 0) {
            body.append(',');
        }
        body.append(signs[index]);
    }
    body.append("],\"ResKind\":0,\"MessageId\":\"SummonDataGetListResponse\"}");
    return QHttpServerResponse{"application/json", body, QHttpServerResponse::StatusCode::Ok};
}

QHttpServerResponse RawJsonResponse(const QByteArray& body, const QByteArray& hostPlacement = {}) {
    QHttpServerResponse response{"application/json", body, QHttpServerResponse::StatusCode::Ok};
    if (!hostPlacement.isEmpty()) {
        QHttpHeaders headers = response.headers();
        if (headers.append(HostPlacementHeader, hostPlacement)) {
            response.setHeaders(std::move(headers));
        }
    }
    return response;
}

QHttpServerResponse InvalidRequest(const QString& reason) {
    QJsonObject body;
    body.insert(QStringLiteral("ResKind"), 1);
    body.insert(QStringLiteral("MessageId"), QString());
    body.insert(QStringLiteral("Error"), reason);
    return QHttpServerResponse{"application/json",
                               QJsonDocument(body).toJson(QJsonDocument::Compact),
                               QHttpServerResponse::StatusCode::BadRequest};
}

std::optional<QJsonObject> ParseRequest(const QHttpServerRequest& request) {
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(request.body(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return std::nullopt;
    }
    return document.object();
}

qint64 Integer(const QJsonObject& object, const QString& key, qint64 fallback = 0) {
    const QJsonValue value = object.value(key);
    return value.isDouble() ? value.toVariant().toLongLong() : fallback;
}

bool EnvEnabled(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' && QString::fromUtf8(value).trimmed() != "0";
}

void TraceSummonRequest(const char* route, const QHttpServerRequest& request) {
    if (!EnvEnabled("SHADNET_BLOODBORNE_RE_TRACE")) {
        return;
    }
    qInfo().noquote() << "Bloodborne summon RE" << route << request.body();
}

// One-bell join (2026-11-10, opt-in: SHADNET_BLOODBORNE_ONE_BELL=1 here, SHADPS4_BBSC_ONE_BELL=1 in
// the clients). Each client says in a request header what it is doing: "host" (a search effect is
// on), "guest" (its own sign is out) or "solo". The server remembers the last host/guest per user
// for 15 s and, on the poll of a SOLO caller, answers with the partner's id when the partner is a
// mutual friend who is searching (host, whose own search found nothing) or advertising (guest).
// The solo caller's game then rings the matching bell itself, so one bell makes a join.
constexpr auto OneBellRoleHeader = "X-ShadPS4-Bloodborne-One-Bell";
constexpr auto PartnerSearchingHeader = "X-ShadPS4-Bloodborne-Partner-Searching";
constexpr auto PartnerAdvertisingHeader = "X-ShadPS4-Bloodborne-Partner-Advertising";
constexpr qint64 OneBellFreshMs = 15'000;

class OneBell {
public:
    explicit OneBell(Database* db) : m_db(db) {}

    // role: "host", "guest" or anything else (= solo). hostFoundNothing: a host's search that
    // returned no sign (a host that found one is being served by the normal path).
    // Returns the header name/value to answer with, empty name = none.
    std::pair<QByteArray, QByteArray> Poll(qint64 user, const QByteArray& role, bool hostFoundNothing,
                                           qint64 nowMs) {
        if (user < 0 || m_db == nullptr || role.isEmpty()) { // no header = an old client
            if (m_db != nullptr && m_noRoleLogged < 6) { // test aid: the switch is on, a poll came without a role
                ++m_noRoleLogged;
                qInfo() << "Bloodborne one-bell: poll without a role header from user" << user;
            }
            return {};
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_lastRole[user] != role) { // test aid: one line per change of a user's role
            m_lastRole[user] = role;
            qInfo() << "Bloodborne one-bell: user" << user << "role" << role;
        }
        if (role == "host" && hostFoundNothing) {
            m_state[user] = {Role::Searching, nowMs};
        } else if (role == "guest") {
            m_state[user] = {Role::Advertising, nowMs};
        } else {
            m_state.erase(user); // solo, or a host that found a sign: nothing to offer
        }
        if (role != "solo") {
            return {};
        }
        for (const auto& [other, entry] : m_state) {
            if (other == user || nowMs - entry.atMs > OneBellFreshMs) {
                continue;
            }
            if (!MutualFriends(user, other)) {
                if (m_notFriendsLogged < 6) {
                    ++m_notFriendsLogged;
                    qInfo() << "Bloodborne one-bell: users" << user << "and" << other
                            << "are not mutual friends in the database: no answer";
                }
                continue;
            }
            return {entry.role == Role::Searching ? PartnerSearchingHeader
                                                  : PartnerAdvertisingHeader,
                    QByteArray::number(other)};
        }
        return {};
    }

private:
    enum class Role { Searching, Advertising };
    struct Entry {
        Role role;
        qint64 atMs;
    };

    bool MutualFriends(qint64 a, qint64 b) {
        const auto [result, status] = m_db->GetRelStatus(a, b);
        const auto friendBit = static_cast<uint8_t>(FriendStatus::Friend);
        return result == Database::RelResult::Ok && (status.caller & friendBit) &&
               (status.other & friendBit);
    }

    Database* m_db;
    std::mutex m_mutex;
    std::map<qint64, Entry> m_state;
    std::map<qint64, QByteArray> m_lastRole;
    int m_noRoleLogged = 0;
    int m_notFriendsLogged = 0;
};

void AddHeader(QHttpServerResponse& response, const QByteArray& name, const QByteArray& value) {
    if (name.isEmpty()) {
        return;
    }
    QHttpHeaders headers = response.headers();
    if (headers.append(name, value)) {
        response.setHeaders(std::move(headers));
    }
}

} // namespace

void RegisterBloodborneRoutes(QHttpServer& http, bool seamlessCoop, Database* db) {
    seamlessCoop = seamlessCoop || EnvEnabled("SHADNET_BLOODBORNE_SEAMLESS_COOP");
    Bloodborne::SummonBroker::Options options;
    options.seamlessCoop = seamlessCoop;
    auto broker = std::make_shared<Bloodborne::SummonBroker>(options);
    // Off unless SHADNET_BLOODBORNE_ONE_BELL is set (and a database is there for the friend check).
    const auto oneBell = std::make_shared<OneBell>(EnvEnabled("SHADNET_BLOODBORNE_ONE_BELL") ? db : nullptr);

    qInfo() << "Bloodborne summon routes registered; seamless co-op"
            << (broker->IsSeamlessCoopEnabled() ? "enabled" : "disabled") << "anywhere summons"
            << (broker->IsSeamlessAnywhereSummonsEnabled() ? "enabled" : "disabled");

    http.route("/summon_messenger/create", QHttpServerRequest::Method::Post,
               [broker, oneBell](const QHttpServerRequest& request) -> QHttpServerResponse {
                   TraceSummonRequest("create", request);
                   const auto body = ParseRequest(request);
                   if (!body || !Bloodborne::HasRequiredAdvertisementFields(*body)) {
                       return InvalidRequest(QStringLiteral("Invalid summon advertisement"));
                   }
                   const auto result = broker->Advertise(*body, request.body(),
                                                         QDateTime::currentMSecsSinceEpoch());
                   if (!result.pendingClaim.isEmpty()) {
                       const QByteArray response =
                           Bloodborne::BuildClaimDeliveryResponse(result.pendingClaim);
                       if (response.isEmpty()) {
                           return InvalidRequest(QStringLiteral("Invalid pending summon claim"));
                       }
                       qInfo() << "Bloodborne summon: delivered claim to user"
                               << Integer(*body, QStringLiteral("UserId")) << "session"
                               << body->value(QStringLiteral("SessionId")).toString()
                               << "host-placement-bytes" << result.pendingHostPlacement.size();
                       return RawJsonResponse(response, result.pendingHostPlacement);
                   }
                   if (!result.pendingHostPlacement.isEmpty()) {
                       qInfo() << "Bloodborne summon: preparing cross-map user"
                               << Integer(*body, QStringLiteral("UserId")) << "session"
                               << body->value(QStringLiteral("SessionId")).toString()
                               << "destination placement bytes"
                               << result.pendingHostPlacement.size();
                       return SummonResponse(QStringLiteral("SummonDataCreateResponse"),
                                             result.pendingHostPlacement);
                   }
                   qInfo() << "Bloodborne summon: advertised user"
                           << Integer(*body, QStringLiteral("UserId")) << "session"
                           << body->value(QStringLiteral("SessionId")).toString();
                   QHttpServerResponse created =
                       SummonResponse(QStringLiteral("SummonDataCreateResponse"));
                   // One-bell: a sign is out, so a searching friend's next poll hears about it.
                   const auto [name, value] =
                       oneBell->Poll(Integer(*body, QStringLiteral("UserId"), -1),
                                     request.value(OneBellRoleHeader), false,
                                     QDateTime::currentMSecsSinceEpoch());
                   AddHeader(created, name, value);
                   return created;
               });

    http.route("/summon_messenger/get", QHttpServerRequest::Method::Post,
               [broker, oneBell](const QHttpServerRequest& request) -> QHttpServerResponse {
                   TraceSummonRequest("get", request);
                   const auto body = ParseRequest(request);
                   if (!body) {
                       return InvalidRequest(QStringLiteral("Invalid summon search"));
                   }
                   const QList<QByteArray> signs =
                       broker->Search(*body, QDateTime::currentMSecsSinceEpoch(),
                                      request.value(HostPlacementHeader));
                   qInfo() << "Bloodborne summon: search for user"
                           << Integer(*body, QStringLiteral("UserId")) << "returned"
                           << signs.size();
                   QHttpServerResponse listed = SummonListResponse(signs);
                   const auto [name, value] =
                       oneBell->Poll(Integer(*body, QStringLiteral("UserId"), -1),
                                     request.value(OneBellRoleHeader), signs.isEmpty(),
                                     QDateTime::currentMSecsSinceEpoch());
                   if (!name.isEmpty()) {
                       qInfo() << "Bloodborne one-bell: answering user"
                               << Integer(*body, QStringLiteral("UserId")) << name << value;
                   }
                   AddHeader(listed, name, value);
                   return listed;
               });

    http.route("/summon_messenger/delete", QHttpServerRequest::Method::Post,
               [broker](const QHttpServerRequest& request) -> QHttpServerResponse {
                   TraceSummonRequest("delete", request);
                   const auto body = ParseRequest(request);
                   if (!body) {
                       return InvalidRequest(QStringLiteral("Invalid summon removal"));
                   }
                   const auto result = broker->Consume(*body, QDateTime::currentMSecsSinceEpoch());
                   qInfo() << "Bloodborne summon: consumed" << result.consumed << "retained"
                           << result.retained << "advertisement(s) host-placement-bytes"
                           << result.pendingHostPlacement.size();
                   QHttpServerResponse response =
                       SummonResponse(QStringLiteral("SummonDataRemoveResponse"));
                   if (result.pendingHostPlacement.isEmpty()) {
                       return response;
                   }
                   QHttpHeaders headers = response.headers();
                   if (headers.append(HostPlacementHeader, result.pendingHostPlacement)) {
                       response.setHeaders(std::move(headers));
                   }
                   return response;
               });

    http.route("/summon_messenger/request", QHttpServerRequest::Method::Post,
               [broker](const QHttpServerRequest& request) -> QHttpServerResponse {
                   TraceSummonRequest("request", request);
                   const auto body = ParseRequest(request);
                   if (!body) {
                       return InvalidRequest(QStringLiteral("Invalid summon request"));
                   }
                   const auto result =
                       broker->Claim(*body, request.body(), QDateTime::currentMSecsSinceEpoch(),
                                     request.value(HostPlacementHeader));
                   switch (result.status) {
                   case Bloodborne::SummonBroker::ClaimStatus::Claimed:
                       qInfo() << "Bloodborne summon: claimed session" << result.targetSessionId
                               << "user" << result.targetUserId;
                       break;
                   case Bloodborne::SummonBroker::ClaimStatus::AlreadyClaimed:
                       qInfo() << "Bloodborne summon: repeated claim for session"
                               << result.targetSessionId;
                       break;
                   case Bloodborne::SummonBroker::ClaimStatus::NotFound:
                       qWarning() << "Bloodborne summon: claim target not found";
                       break;
                   case Bloodborne::SummonBroker::ClaimStatus::Conflict:
                       qWarning() << "Bloodborne summon: conflicting claim for session"
                                  << result.targetSessionId;
                       break;
                   }
                   return SummonResponse(QStringLiteral("SummonDataSummonResponse"));
               });
}

} // namespace WebApiRoutes
