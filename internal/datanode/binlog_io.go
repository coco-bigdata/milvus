// Licensed to the LF AI & Data foundation under one
// or more contributor license agreements. See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership. The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License. You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

package datanode

import (
	"context"
	"errors"
	"path"
	"strconv"
	"time"

	"github.com/milvus-io/milvus/internal/util/metautil"

	"github.com/milvus-io/milvus/internal/common"
	"github.com/milvus-io/milvus/internal/log"
	"github.com/milvus-io/milvus/internal/proto/datapb"
	"github.com/milvus-io/milvus/internal/proto/etcdpb"
	"github.com/milvus-io/milvus/internal/storage"
	"go.uber.org/zap"
	"golang.org/x/sync/errgroup"
)

var (
	errUploadToBlobStorage     = errors.New("upload to blob storage wrong")
	errDownloadFromBlobStorage = errors.New("download from blob storage wrong")
)

type downloader interface {
	// donload downloads insert-binlogs, stats-binlogs, and, delta-binlogs from blob storage for given paths.
	// The paths are 1 group of binlog paths generated by 1 `Serialize`.
	//
	// errDownloadFromBlobStorage is returned if ctx is canceled from outside while a downloading is inprogress.
	// Beware of the ctx here, if no timeout or cancel is applied to this ctx, this downloading may retry forever.
	download(ctx context.Context, paths []string) ([]*Blob, error)
}

type uploader interface {
	// upload saves InsertData and DeleteData into blob storage, stats binlogs are generated from InsertData.
	//
	// errUploadToBlobStorage is returned if ctx is canceled from outside while a uploading is inprogress.
	// Beware of the ctx here, if no timeout or cancel is applied to this ctx, this uploading may retry forever.
	upload(ctx context.Context, segID, partID UniqueID, iData []*InsertData, segStats []byte, dData *DeleteData, meta *etcdpb.CollectionMeta) (*segPaths, error)
}

type binlogIO struct {
	storage.ChunkManager
	allocatorInterface
}

var _ downloader = (*binlogIO)(nil)
var _ uploader = (*binlogIO)(nil)

func (b *binlogIO) download(ctx context.Context, paths []string) ([]*Blob, error) {
	var (
		err = errStart
		vs  [][]byte
	)

	g, gCtx := errgroup.WithContext(ctx)
	g.Go(func() error {
		for err != nil {
			select {

			case <-gCtx.Done():
				log.Warn("ctx done when downloading kvs from blob storage")
				return errDownloadFromBlobStorage

			default:
				if err != errStart {
					log.Warn("downloading failed, retry in 50ms", zap.Strings("paths", paths))
					<-time.After(50 * time.Millisecond)
				}
				vs, err = b.MultiRead(paths)
			}
		}
		return nil
	})

	if err := g.Wait(); err != nil {
		return nil, err
	}

	rst := make([]*Blob, len(vs))
	for i := range rst {
		rst[i] = &Blob{Value: vs[i]}
	}

	return rst, nil
}

type segPaths struct {
	inPaths    []*datapb.FieldBinlog
	statsPaths []*datapb.FieldBinlog
	deltaInfo  []*datapb.FieldBinlog
}

func (b *binlogIO) upload(
	ctx context.Context,
	segID, partID UniqueID,
	iDatas []*InsertData,
	segStats []byte,
	dData *DeleteData,
	meta *etcdpb.CollectionMeta) (*segPaths, error) {

	var (
		p   = &segPaths{}             // The returns
		kvs = make(map[string][]byte) // Key values to store in minIO

		insertField2Path = make(map[UniqueID]*datapb.FieldBinlog) // FieldID > its FieldBinlog
		statsField2Path  = make(map[UniqueID]*datapb.FieldBinlog) // FieldID > its statsBinlog
	)

	for _, iData := range iDatas {
		tf, ok := iData.Data[common.TimeStampField]
		if !ok || tf.RowNum() == 0 {
			log.Warn("binlog io uploading empty insert data",
				zap.Int64("segmentID", segID),
				zap.Int64("collectionID", meta.GetID()),
			)
			continue
		}

		kv, inpaths, err := b.genInsertBlobs(iData, partID, segID, meta)
		if err != nil {
			log.Warn("generate insert blobs wrong",
				zap.Int64("collectionID", meta.GetID()),
				zap.Int64("segmentID", segID),
				zap.Error(err))
			return nil, err
		}

		for k, v := range kv {
			kvs[k] = v
		}

		for fID, path := range inpaths {
			tmpBinlog, ok := insertField2Path[fID]
			if !ok {
				tmpBinlog = path
			} else {
				tmpBinlog.Binlogs = append(tmpBinlog.Binlogs, path.GetBinlogs()...)
			}
			insertField2Path[fID] = tmpBinlog
		}
	}

	pkID := getPKID(meta)
	if pkID == common.InvalidFieldID {
		log.Error("get invalid field id when finding pk", zap.Int64("collectionID", meta.GetID()), zap.Any("fields", meta.GetSchema().GetFields()))
		return nil, errors.New("invalid pk id")
	}
	logID, err := b.allocID()
	if err != nil {
		return nil, err
	}
	k := metautil.JoinIDPath(meta.GetID(), partID, segID, pkID, logID)
	key := path.Join(b.ChunkManager.RootPath(), common.SegmentStatslogPath, k)
	fileLen := len(segStats)

	kvs[key] = segStats
	statsField2Path[pkID] = &datapb.FieldBinlog{
		FieldID: pkID,
		Binlogs: []*datapb.Binlog{{LogSize: int64(fileLen), LogPath: key}},
	}

	for _, path := range insertField2Path {
		p.inPaths = append(p.inPaths, path)
	}

	for _, path := range statsField2Path {
		p.statsPaths = append(p.statsPaths, path)
	}

	// If there are delta binlogs
	if dData.RowCount > 0 {
		k, v, err := b.genDeltaBlobs(dData, meta.GetID(), partID, segID)
		if err != nil {
			log.Warn("generate delta blobs wrong",
				zap.Int64("collectionID", meta.GetID()),
				zap.Int64("segmentID", segID),
				zap.Error(err))
			return nil, err
		}

		kvs[k] = v
		p.deltaInfo = append(p.deltaInfo, &datapb.FieldBinlog{
			FieldID: 0, // TODO: Not useful on deltalogs, FieldID shall be ID of primary key field
			Binlogs: []*datapb.Binlog{{
				EntriesNum: dData.RowCount,
				LogPath:    k,
				LogSize:    int64(len(v)),
			}},
		})
	}

	err = errStart
	for err != nil {
		select {
		case <-ctx.Done():
			log.Warn("ctx done when saving kvs to blob storage",
				zap.Int64("collectionID", meta.GetID()),
				zap.Int64("segmentID", segID),
				zap.Int("number of kvs", len(kvs)))
			return nil, errUploadToBlobStorage
		default:
			if err != errStart {
				log.Warn("save binlog failed, retry in 50ms",
					zap.Int64("collectionID", meta.GetID()),
					zap.Int64("segmentID", segID))
				<-time.After(50 * time.Millisecond)
			}
			err = b.MultiWrite(kvs)
		}
	}
	return p, nil
}

// genDeltaBlobs returns key, value
func (b *binlogIO) genDeltaBlobs(data *DeleteData, collID, partID, segID UniqueID) (string, []byte, error) {
	dCodec := storage.NewDeleteCodec()

	blob, err := dCodec.Serialize(collID, partID, segID, data)
	if err != nil {
		return "", nil, err
	}

	k, err := b.genKey(collID, partID, segID)
	if err != nil {
		return "", nil, err
	}

	key := path.Join(b.ChunkManager.RootPath(), common.SegmentDeltaLogPath, k)

	return key, blob.GetValue(), nil
}

// genInsertBlobs returns kvs, insert-paths, stats-paths
func (b *binlogIO) genInsertBlobs(data *InsertData, partID, segID UniqueID, meta *etcdpb.CollectionMeta) (map[string][]byte, map[UniqueID]*datapb.FieldBinlog, error) {
	inCodec := storage.NewInsertCodec(meta)
	inlogs, _, err := inCodec.Serialize(partID, segID, data)
	if err != nil {
		return nil, nil, err
	}

	var (
		kvs     = make(map[string][]byte, len(inlogs)+1)
		inpaths = make(map[UniqueID]*datapb.FieldBinlog)
	)

	notifyGenIdx := make(chan struct{})
	defer close(notifyGenIdx)

	generator, err := b.idxGenerator(len(inlogs)+1, notifyGenIdx)
	if err != nil {
		return nil, nil, err
	}

	for _, blob := range inlogs {
		// Blob Key is generated by Serialize from int64 fieldID in collection schema, which won't raise error in ParseInt
		fID, _ := strconv.ParseInt(blob.GetKey(), 10, 64)
		k := metautil.JoinIDPath(meta.GetID(), partID, segID, fID, <-generator)
		key := path.Join(b.ChunkManager.RootPath(), common.SegmentInsertLogPath, k)

		value := blob.GetValue()
		fileLen := len(value)

		kvs[key] = value
		inpaths[fID] = &datapb.FieldBinlog{
			FieldID: fID,
			Binlogs: []*datapb.Binlog{{LogSize: int64(fileLen), LogPath: key}},
		}
	}

	return kvs, inpaths, nil
}

func (b *binlogIO) idxGenerator(n int, done <-chan struct{}) (<-chan UniqueID, error) {

	idStart, _, err := b.allocIDBatch(uint32(n))
	if err != nil {
		return nil, err
	}

	rt := make(chan UniqueID)
	go func(rt chan<- UniqueID) {
		for i := 0; i < n; i++ {
			select {
			case <-done:
				close(rt)
				return
			case rt <- idStart + UniqueID(i):
			}
		}
		close(rt)
	}(rt)

	return rt, nil
}
