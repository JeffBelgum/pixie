/*
 * Copyright 2018- The Pixie Authors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

package main

import (
	"net/http"
	_ "net/http/pprof"

	log "github.com/sirupsen/logrus"

	bindata "github.com/golang-migrate/migrate/source/go_bindata"

	"px.dev/pixie/src/cloud/shared/pgmigrate"
	"px.dev/pixie/src/e2e_test/perf_tool/backend/coordinator/controllers"
	"px.dev/pixie/src/e2e_test/perf_tool/backend/coordinator/coordinatorenv"
	"px.dev/pixie/src/e2e_test/perf_tool/backend/coordinator/coordinatorpb"
	"px.dev/pixie/src/e2e_test/perf_tool/backend/coordinator/datastore"
	"px.dev/pixie/src/e2e_test/perf_tool/backend/coordinator/schema"
	"px.dev/pixie/src/shared/services"
	"px.dev/pixie/src/shared/services/healthz"
	"px.dev/pixie/src/shared/services/pg"
	"px.dev/pixie/src/shared/services/server"
)

func main() {
	services.SetupService("coordinator-service", 50000)
	services.PostFlagSetupAndParse()
	services.SetupServiceLogging()

	mux := http.NewServeMux()
	healthz.RegisterDefaultChecks(mux)

	db := pg.MustConnectDefaultPostgresDB()
	err := pgmigrate.PerformMigrationsUsingBindata(db, "coordinator_service_migrations",
		bindata.Resource(schema.AssetNames(), schema.Asset))
	if err != nil {
		log.WithError(err).Fatal("Failed to apply migrations")
	}

	datastore := datastore.NewDatastore(db)

	clusterMgrClient, err := coordinatorenv.NewClusterManagerServiceClient()
	if err != nil {
		log.WithError(err).Fatal("failed to connect to clustermgr service")
	}
	runnerClient, err := coordinatorenv.NewRunnerServiceClient()
	if err != nil {
		log.WithError(err).Fatal("failed to connect to runner service")
	}
	builderClient, err := coordinatorenv.NewBuilderServiceClient()
	if err != nil {
		log.WithError(err).Fatal("failed to connect to builder service")
	}

	_ = clusterMgrClient
	_ = runnerClient
	_ = builderClient

	svr := controllers.NewServer(datastore)
	s := server.NewPLServerWithOptions(nil, mux, &server.GRPCServerOptions{
		DisableMiddleware: true,
	})
	coordinatorpb.RegisterCoordinatorServiceServer(s.GRPCServer(), svr)
	coordinatorpb.RegisterBuildNotificationServiceServer(s.GRPCServer(), svr)
	coordinatorpb.RegisterClusterNotificationServiceServer(s.GRPCServer(), svr)
	coordinatorpb.RegisterRunnerNotificationServiceServer(s.GRPCServer(), svr)

	s.Start()
	s.StopOnInterrupt()
}
