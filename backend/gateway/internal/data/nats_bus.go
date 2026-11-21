package data

import (
	"context"
	"fmt"
	"time"

	devicev1 "esp-pika/backend/gateway/api/device/v1"
	eventsv1 "esp-pika/backend/gateway/api/events/v1"
	"esp-pika/backend/gateway/internal/biz"

	"github.com/go-kratos/kratos/v2/log"
	"google.golang.org/protobuf/proto"
	"google.golang.org/protobuf/types/known/timestamppb"
)

type natsEventBus struct {
	data *Data
	log  *log.Helper
}

// NewNATSEventBus constructs an EventBus backed by NATS (degrades gracefully to no-op if NATS is offline).
func NewNATSEventBus(data *Data, logger log.Logger) biz.EventBus {
	return &natsEventBus{
		data: data,
		log:  log.NewHelper(logger),
	}
}

func (b *natsEventBus) publishProto(ctx context.Context, subject string, msg proto.Message) error {
	if b.data.nc == nil {
		return nil
	}
	payload, err := proto.Marshal(msg)
	if err != nil {
		return fmt.Errorf("marshal %s: %w", subject, err)
	}
	if err := b.data.nc.Publish(subject, payload); err != nil {
		b.log.WithContext(ctx).Warnf("nats publish %s failed: %v", subject, err)
		return err
	}
	return nil
}

func (b *natsEventBus) PublishPresence(ctx context.Context, deviceID, groupID, zone string, online bool) error {
	subject := fmt.Sprintf("esp_pika.events.presence.%s", groupID)
	return b.publishProto(ctx, subject, &eventsv1.DevicePresenceEvent{
		DeviceId:   deviceID,
		GroupId:    groupID,
		Zone:       zone,
		Online:     online,
		OccurredAt: timestamppb.New(time.Now().UTC()),
	})
}

func (b *natsEventBus) PublishWakeDecision(ctx context.Context, decision *biz.WakeDecision) error {
	subject := fmt.Sprintf("esp_pika.events.wake_decision.%s", decision.GroupID)
	return b.publishProto(ctx, subject, &eventsv1.WakeDecision{
		GroupId:        decision.GroupID,
		WinnerDeviceId: decision.WinnerDeviceID,
		TargetDeviceId: decision.TargetDeviceID,
		Accepted:       decision.Accepted,
		Reason:         decision.Reason,
		DecidedAtMs:    decision.DecidedAtMS,
	})
}

func (b *natsEventBus) PublishVoiceprintSync(ctx context.Context, syncEvt *biz.VoiceprintSync) error {
	subject := fmt.Sprintf("esp_pika.events.voiceprint_sync.%s", syncEvt.GroupID)
	upserts := make([]*eventsv1.VoiceprintTemplate, 0, len(syncEvt.Upserts))
	for _, u := range syncEvt.Upserts {
		upserts = append(upserts, &eventsv1.VoiceprintTemplate{
			SpeakerId:   u.SpeakerID,
			DisplayName: u.DisplayName,
			Role:        devicev1.SpeakerRole(u.Role),
			Embedding:   u.Embedding,
			Version:     u.Version,
		})
	}
	return b.publishProto(ctx, subject, &eventsv1.VoiceprintSyncEvent{
		GroupId:           syncEvt.GroupID,
		VpVersion:         syncEvt.VPVersion,
		FullSync:          syncEvt.FullSync,
		Upserts:           upserts,
		DeletedSpeakerIds: syncEvt.DeletedSpeakerIDs,
	})
}

func (b *natsEventBus) PublishSensorPush(ctx context.Context, deviceID, groupID, zone, eventType, payloadJSON string) error {
	subject := fmt.Sprintf("esp_pika.events.sensor_push.%s", groupID)
	return b.publishProto(ctx, subject, &eventsv1.SensorPushEvent{
		DeviceId:    deviceID,
		GroupId:     groupID,
		Zone:        zone,
		EventType:   eventType,
		PayloadJson: payloadJSON,
		OccurredAt:  timestamppb.New(time.Now().UTC()),
	})
}
