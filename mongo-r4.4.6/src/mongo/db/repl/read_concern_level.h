/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <boost/optional.hpp>

namespace mongo {
namespace repl {


//���� https://docs.mongodb.com/v4.4/reference/read-concern/
//�ο� https://mongoing.com/archives/77853

//ע��ReadConcernLevel��ReadSource�е�RecoveryUnit::kMajorityCommitted�ȹ�����ת���ο�getNewReadSource

//���մ洢��ReadConcernArgs._level��
enum class ReadConcernLevel {
    //local/available��local �� available ���������һ�£����Ƕ�����ֱ�Ӷ�ȡ�������µ����ݡ����ǣ�available ʹ��
    // MongoDB ��Ƭ��Ⱥ�����£����������壨Ϊ�˱�֤���ܣ����Է��ع¶��ĵ�����
    //��Ч��canReadAtLastApplied
    kLocalReadConcern,
    //��Ч��waitForReadConcernImpl  ReplicationCoordinatorImpl::_waitUntilClusterTimeForRead  computeOperationTime
    kMajorityReadConcern,
    //��Ч�ο�service_entry_point_mongod.cpp�е�waitForLinearizableReadConcern�е���
    //ֱ�Ӷ����ڵ�
    kLinearizableReadConcern,
    //available ʹ���ڷ�Ƭ��Ⱥ�����£���kLocalReadConcern�������ƣ���Ч��canReadAtLastApplied
    kAvailableReadConcern,
    kSnapshotReadConcern
};

namespace readConcernLevels {

//���� https://docs.mongodb.com/v4.4/reference/read-concern/
//�ο� https://mongoing.com/archives/77853
constexpr std::initializer_list<ReadConcernLevel> all = {ReadConcernLevel::kLocalReadConcern,
                                                         ReadConcernLevel::kMajorityReadConcern,
                                                         ReadConcernLevel::kLinearizableReadConcern,
                                                         ReadConcernLevel::kAvailableReadConcern,
                                                         ReadConcernLevel::kSnapshotReadConcern};


//���� https://docs.mongodb.com/v4.2/reference/read-concern/
//�ο� https://mongoing.com/archives/77853
constexpr StringData kLocalName = "local"_sd;
constexpr StringData kMajorityName = "majority"_sd;
constexpr StringData kLinearizableName = "linearizable"_sd;
constexpr StringData kAvailableName = "available"_sd;
constexpr StringData kSnapshotName = "snapshot"_sd;

boost::optional<ReadConcernLevel> fromString(StringData levelString);
StringData toString(ReadConcernLevel level);

}  // namespace readConcernLevels

}  // namespace repl
}  // namespace mongo
