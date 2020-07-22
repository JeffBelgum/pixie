package controllers

import (
	"context"
	"errors"
	"time"

	uuid "github.com/satori/go.uuid"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"
	statuspb "pixielabs.ai/pixielabs/src/common/base/proto"
	schemapb "pixielabs.ai/pixielabs/src/table_store/proto"
	"pixielabs.ai/pixielabs/src/utils"
	"pixielabs.ai/pixielabs/src/vizier/services/metadata/metadataenv"
	"pixielabs.ai/pixielabs/src/vizier/services/metadata/metadatapb"
	storepb "pixielabs.ai/pixielabs/src/vizier/services/metadata/storepb"
	agentpb "pixielabs.ai/pixielabs/src/vizier/services/shared/agentpb"
)

// UnhealthyAgentThreshold is the amount of time where an agent is considered unhealthy if
// its last heartbeat is greater than this value.
const UnhealthyAgentThreshold = 30 * time.Second

// Server defines an gRPC server type.
type Server struct {
	env          metadataenv.MetadataEnv
	agentManager AgentManager
	probeManager *ProbeManager
	clock        utils.Clock
	mds          MetadataStore
}

// NewServerWithClock creates a new server with a clock.
func NewServerWithClock(env metadataenv.MetadataEnv, agtMgr AgentManager, probeManager *ProbeManager, mds MetadataStore, clock utils.Clock) (*Server, error) {
	return &Server{
		env:          env,
		agentManager: agtMgr,
		probeManager: probeManager,
		clock:        clock,
		mds:          mds,
	}, nil
}

// NewServer creates GRPC handlers.
func NewServer(env metadataenv.MetadataEnv, agtMgr AgentManager, probeManager *ProbeManager, mds MetadataStore) (*Server, error) {
	clock := utils.SystemClock{}
	return NewServerWithClock(env, agtMgr, probeManager, mds, clock)
}

func (s *Server) getSchemas() (*schemapb.Schema, error) {
	schemas, err := s.mds.GetComputedSchemas()
	if err != nil {
		return nil, err
	}
	respSchemaPb := &schemapb.Schema{}
	respSchemaPb.RelationMap = make(map[string]*schemapb.Relation)

	for _, schema := range schemas {
		columnPbs := make([]*schemapb.Relation_ColumnInfo, len(schema.Columns))
		for j, column := range schema.Columns {
			columnPbs[j] = &schemapb.Relation_ColumnInfo{
				ColumnName:         column.Name,
				ColumnType:         column.DataType,
				ColumnDesc:         column.Desc,
				ColumnSemanticType: column.SemanticType,
			}
		}
		schemaPb := &schemapb.Relation{
			Columns: columnPbs,
		}
		respSchemaPb.RelationMap[schema.Name] = schemaPb
	}

	return respSchemaPb, nil
}

// GetSchemas returns the schemas in the system.
func (s *Server) GetSchemas(ctx context.Context, req *metadatapb.SchemaRequest) (*metadatapb.SchemaResponse, error) {
	respSchemaPb, err := s.getSchemas()
	if err != nil {
		return nil, err
	}
	resp := &metadatapb.SchemaResponse{
		Schema: respSchemaPb,
	}
	return resp, nil
}

// GetSchemaByAgent returns the schemas in the system.
func (s *Server) GetSchemaByAgent(ctx context.Context, req *metadatapb.SchemaByAgentRequest) (*metadatapb.SchemaByAgentResponse, error) {
	return nil, status.Error(codes.Unimplemented, "Not implemented yet")
}

// GetAgentInfo returns information about registered agents.
func (s *Server) GetAgentInfo(ctx context.Context, req *metadatapb.AgentInfoRequest) (*metadatapb.AgentInfoResponse, error) {
	agents, err := s.agentManager.GetActiveAgents()
	if err != nil {
		return nil, err
	}

	currentTime := s.clock.Now()

	// Populate AgentInfoResponse.
	agentResponses := make([]*metadatapb.AgentMetadata, 0)
	for _, agent := range agents {
		state := agentpb.AGENT_STATE_HEALTHY
		timeSinceLastHb := currentTime.Sub(time.Unix(0, agent.LastHeartbeatNS))
		if timeSinceLastHb > UnhealthyAgentThreshold {
			state = agentpb.AGENT_STATE_UNRESPONSIVE
		}

		resp := metadatapb.AgentMetadata{
			Agent: agent,
			Status: &agentpb.AgentStatus{
				NSSinceLastHeartbeat: timeSinceLastHb.Nanoseconds(),
				State:                state,
			},
		}
		agentResponses = append(agentResponses, &resp)
	}

	resp := metadatapb.AgentInfoResponse{
		Info: agentResponses,
	}

	return &resp, nil
}

// GetAgentTableMetadata returns table metadata for each agent. We currently assume that all agents
// have the same schema, but this code will need to be updated when that assumption no longer holds true.
func (s *Server) GetAgentTableMetadata(ctx context.Context, req *metadatapb.AgentTableMetadataRequest) (*metadatapb.AgentTableMetadataResponse, error) {
	respSchemaPb, err := s.getSchemas()
	if err != nil {
		return nil, err
	}
	dataInfos, err := s.mds.GetAgentsDataInfo()
	if err != nil {
		return nil, err
	}

	// Populate AgentTableMetadataResponse.
	var agentsMetadata []*metadatapb.AgentTableMetadata
	for agentID, dataInfo := range dataInfos {
		a := &metadatapb.AgentTableMetadata{
			AgentID:  utils.ProtoFromUUID(&agentID),
			Schema:   respSchemaPb,
			DataInfo: dataInfo,
		}
		agentsMetadata = append(agentsMetadata, a)
	}

	resp := metadatapb.AgentTableMetadataResponse{
		MetadataByAgent: agentsMetadata,
	}

	return &resp, nil
}

// RegisterProbe is a request to register the probes specified in the Program on all agents.
func (s *Server) RegisterProbe(ctx context.Context, req *metadatapb.RegisterProbeRequest) (*metadatapb.RegisterProbeResponse, error) {
	// Create probe.
	probeID, err := s.probeManager.CreateProbe(req.ProbeName, req.Program)
	if err != nil && err != ErrProbeAlreadyExists {
		return nil, err
	}
	if err == ErrProbeAlreadyExists {
		return &metadatapb.RegisterProbeResponse{
			ProbeID: req.ProbeName,
			Status: &statuspb.Status{
				ErrCode: statuspb.ALREADY_EXISTS,
			},
		}, nil
	}

	// Get all agents currently running.
	agents, err := s.agentManager.GetActiveAgents()
	if err != nil {
		return nil, err
	}
	agentIDs := make([]uuid.UUID, len(agents))
	for i, agent := range agents {
		agentIDs[i] = utils.UUIDFromProtoOrNil(agent.Info.AgentID)
	}

	// Register probe on all agents.
	err = s.probeManager.RegisterProbe(agentIDs, probeID, req.Program)
	if err != nil {
		return nil, err
	}

	resp := &metadatapb.RegisterProbeResponse{
		ProbeID: probeID,
		Status: &statuspb.Status{
			ErrCode: statuspb.OK,
		},
	}

	return resp, nil
}

// GetProbeInfo is a request to check the status for the given probe.
func (s *Server) GetProbeInfo(ctx context.Context, req *metadatapb.GetProbeInfoRequest) (*metadatapb.GetProbeInfoResponse, error) {
	probeState := make([]*metadatapb.GetProbeInfoResponse_ProbeState, len(req.ProbeIDs))

	for i, probeID := range req.ProbeIDs {
		probe, err := s.probeManager.GetProbeInfo(probeID)
		if err != nil {
			return nil, err
		}
		if probe == nil { // Probe does not exist.
			probeState[i] = &metadatapb.GetProbeInfoResponse_ProbeState{
				ProbeID: probeID,
				State:   statuspb.UNKNOWN_STATE,
				Status: &statuspb.Status{
					ErrCode: statuspb.NOT_FOUND,
				},
			}
			continue
		}

		probeStates, err := s.probeManager.GetProbeStates(probeID)
		if err != nil {
			return nil, err
		}

		state, status := getProbeStateFromAgentProbeStates(probeStates)

		probeState[i] = &metadatapb.GetProbeInfoResponse_ProbeState{
			ProbeID: probeID,
			State:   state,
			Status:  status,
		}
	}

	return &metadatapb.GetProbeInfoResponse{
		Probes: probeState,
	}, nil
}

func getProbeStateFromAgentProbeStates(agentStates []*storepb.AgentProbeStatus) (statuspb.LifeCycleState, *statuspb.Status) {
	if len(agentStates) == 0 {
		return statuspb.PENDING_STATE, nil
	}

	numFailed := 0
	numEvicted := 0
	numPending := 0
	numRunning := 0

	for _, s := range agentStates {
		if s.State == statuspb.EVICTED_STATE {
			numEvicted++
		} else if s.State == statuspb.FAILED_STATE {
			numFailed++
		} else if s.State == statuspb.PENDING_STATE {
			numPending++
		} else if s.State == statuspb.RUNNING_STATE {
			numRunning++
		}
	}

	if numEvicted > 0 { // If any agentProbes are evicted, then we consider the probe in an evicted state.
		return statuspb.EVICTED_STATE, nil
	}

	if numRunning > 0 { // If a single agentProbe is running, then we consider the overall probe as healthy.
		return statuspb.RUNNING_STATE, nil
	}

	if numPending > 0 { // If no agentProbes are running, but some are in a pending state, the probe is pending.
		return statuspb.PENDING_STATE, nil
	}

	if numFailed > 0 { // If there are no evicted/running/pending probes, then the probe is failed.
		return statuspb.FAILED_STATE, agentStates[0].Status // Just use the status from the first failed agent for now.
	}

	return statuspb.UNKNOWN_STATE, nil
}

// EvictProbe is a request to evict the given probe on all agents.
func (s *Server) EvictProbe(ctx context.Context, req *metadatapb.EvictProbeRequest) (*metadatapb.EvictProbeResponse, error) {
	return nil, errors.New("Not yet implemented")
}
