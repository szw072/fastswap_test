#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include "fastswap_rdma.h"
#include <linux/slab.h>
#include <linux/cpumask.h>
#include <linux/string.h>

#include <linux/random.h>
#include <linux/crypto.h> 
#include <linux/crc16.h>

static struct sswap_rdma_ctrl *gctrl;
static int serverport;
static int numqueues;
static int numcpus;
static char serverip[INET_ADDRSTRLEN];
static char clientip[INET_ADDRSTRLEN];
static struct kmem_cache *req_cache;
module_param_named(sport, serverport, int, 0644);

// modified by ysjing
// module_param_named(nq, numqueues, int, 0644);
module_param_named(nc, numcpus, int, 0644);

module_param_string(sip, serverip, INET_ADDRSTRLEN, 0644);
module_param_string(cip, clientip, INET_ADDRSTRLEN, 0644);

// TODO: destroy ctrl

#define CONNECTION_TIMEOUT_MS 60000
#define QP_QUEUE_DEPTH 256
/* we don't really use recv wrs, so any small number should do */
#define QP_MAX_RECV_WR 4
/* we mainly do send wrs */
#define QP_MAX_SEND_WR	(4096)
#define CQ_NUM_CQES	(QP_MAX_SEND_WR)
#define POLL_BATCH_HIGH (QP_MAX_SEND_WR / 4)


static int compress(void* src,unsigned int slen, void *dst, unsigned int *dlen){
  int ret;
  struct crypto_comp *tfm;
  char alg[] = "lzo";
  tfm = crypto_alloc_comp(alg,0,0);
  if (IS_ERR_OR_NULL(tfm)) {
		pr_err("could not alloc crypto comp");
		return -ENOMEM;
	}
  ret = crypto_comp_compress(tfm, src, PAGE_SIZE, dst, dlen);

  //TODO: 这里不一定合理 参见cpuhp_setup_state_multi
  crypto_free_comp(tfm);//释放crypto_comp对象
  return ret;
}

static int decompress(void* src,int slen, void *dst, unsigned int *dlen){
  int ret;
  struct crypto_comp *tfm;
  char alg[] = "lzo";
  tfm = crypto_alloc_comp(alg,0,0);
  if (IS_ERR_OR_NULL(tfm)) {
		pr_err("could not alloc crypto comp");
		return -ENOMEM;
	}
  ret = crypto_comp_decompress(tfm, src, slen, dst, dlen);
    // pr_info("[decompress] length %d", dlen);
  // if((*dlen) != 4096){//如果解压缩大小不是4KB 触发BUG
  //   pr_info("decompress wrong! len: %d --> %u", slen, *dlen);
  //   BUG();
  // }
  //TODO: 这里不一定合理 参见cpuhp_setup_state_multi
  crypto_free_comp(tfm);//释放crypto_comp对象
  return ret;
}

static void sswap_rdma_addone(struct ib_device *dev)
{
  pr_info("sswap_rdma_addone() = %s\n", dev->name);
}

static void sswap_rdma_removeone(struct ib_device *ib_device, void *client_data)
{
  pr_info("sswap_rdma_removeone()\n");
}

static struct ib_client sswap_rdma_ib_client = {
  .name   = "sswap_rdma",
  .add    = sswap_rdma_addone,
  .remove = sswap_rdma_removeone
};

static struct sswap_rdma_dev *sswap_rdma_get_device(struct rdma_queue *q)
{
  struct sswap_rdma_dev *rdev = NULL;

  if (!q->ctrl->rdev) {
    rdev = kzalloc(sizeof(*rdev), GFP_KERNEL);
    if (!rdev) {
      pr_err("no memory\n");
      goto out_err;
    }

    rdev->dev = q->cm_id->device;

    pr_info("selecting device %s\n", rdev->dev->name);

    rdev->pd = ib_alloc_pd(rdev->dev, 0);
    if (IS_ERR(rdev->pd)) {
      pr_err("ib_alloc_pd\n");
      goto out_free_dev;
    }

    if (!(rdev->dev->attrs.device_cap_flags &
          IB_DEVICE_MEM_MGT_EXTENSIONS)) {
      pr_err("memory registrations not supported\n");
      goto out_free_pd;
    }

    q->ctrl->rdev = rdev;
  }

  return q->ctrl->rdev;

out_free_pd:
  ib_dealloc_pd(rdev->pd);
out_free_dev:
  kfree(rdev);
out_err:
  return NULL;
}

static void sswap_rdma_qp_event(struct ib_event *e, void *c)
{
  pr_info("sswap_rdma_qp_event\n");
}

static int sswap_rdma_create_qp(struct rdma_queue *queue)
{
  struct sswap_rdma_dev *rdev = queue->ctrl->rdev;
  struct ib_qp_init_attr init_attr;
  int ret;

  pr_info("start: %s\n", __FUNCTION__);

  memset(&init_attr, 0, sizeof(init_attr));
  init_attr.event_handler = sswap_rdma_qp_event;
  init_attr.cap.max_send_wr = QP_MAX_SEND_WR;
  init_attr.cap.max_recv_wr = QP_MAX_RECV_WR;
  init_attr.cap.max_recv_sge = 1;
  init_attr.cap.max_send_sge = 1;
  init_attr.sq_sig_type = IB_SIGNAL_REQ_WR;
  init_attr.qp_type = IB_QPT_RC;
  init_attr.send_cq = queue->cq;
  init_attr.recv_cq = queue->cq;
  /* just to check if we are compiling against the right headers */
  init_attr.create_flags = IB_QP_EXP_CREATE_ATOMIC_BE_REPLY & 0;

  ret = rdma_create_qp(queue->cm_id, rdev->pd, &init_attr);
  if (ret) {
    pr_err("rdma_create_qp failed: %d\n", ret);
    return ret;
  }

  queue->qp = queue->cm_id->qp;
  return ret;
}

static void sswap_rdma_destroy_queue_ib(struct rdma_queue *q)
{
  struct sswap_rdma_dev *rdev;
  struct ib_device *ibdev;

  pr_info("start: %s\n", __FUNCTION__);

  rdev = q->ctrl->rdev;
  ibdev = rdev->dev;
  //rdma_destroy_qp(q->ctrl->cm_id);
  ib_free_cq(q->cq);
}

static int sswap_rdma_create_queue_ib(struct rdma_queue *q)//创建QP以及CQ
{
  struct ib_device *ibdev = q->ctrl->rdev->dev;
  int ret;
  int comp_vector = 0;

  pr_info("start: %s\n", __FUNCTION__);

  if (q->qp_type == QP_READ_ASYNC)
    q->cq = ib_alloc_cq(ibdev, q, CQ_NUM_CQES,//READ_ASYNC和READ_SYNC区别 poll类型不同
      comp_vector, IB_POLL_SOFTIRQ);
  else
    q->cq = ib_alloc_cq(ibdev, q, CQ_NUM_CQES,
      comp_vector, IB_POLL_DIRECT);

  if (IS_ERR(q->cq)) {
    ret = PTR_ERR(q->cq);
    goto out_err;
  }

  ret = sswap_rdma_create_qp(q);
  if (ret)
    goto out_destroy_ib_cq;

  return 0;

out_destroy_ib_cq:
  ib_free_cq(q->cq);
out_err:
  return ret;
}

static int sswap_rdma_addr_resolved(struct rdma_queue *q)
{
  struct sswap_rdma_dev *rdev = NULL;
  int ret;

  pr_info("start: %s\n", __FUNCTION__);

  rdev = sswap_rdma_get_device(q);
  if (!rdev) {
    pr_err("no device found\n");
    return -ENODEV;
  }

  ret = sswap_rdma_create_queue_ib(q);
  if (ret) {
    return ret;
  }
  //在执行rdma_connect必须执行rdma_resolve_route
  //执行rdma_resolve_route之前必须执行rdma_resolve_addr
  // rdma_resolve_addr --> rdma_resolve_route --> rdma_connect
  ret = rdma_resolve_route(q->cm_id, CONNECTION_TIMEOUT_MS);
  if (ret) {
    pr_err("rdma_resolve_route failed\n");
    sswap_rdma_destroy_queue_ib(q);
  }

  return 0;
}

static int sswap_rdma_route_resolved(struct rdma_queue *q,
    struct rdma_conn_param *conn_params)
{
  struct rdma_conn_param param = {};
  int ret;

  param.qp_num = q->qp->qp_num;
  param.flow_control = 1;
  param.responder_resources = 16;
  param.initiator_depth = 16;
  param.retry_count = 7;
  param.rnr_retry_count = 7;
  param.private_data = NULL;
  param.private_data_len = 0;

  pr_info("max_qp_rd_atom=%d max_qp_init_rd_atom=%d\n",
      q->ctrl->rdev->dev->attrs.max_qp_rd_atom,
      q->ctrl->rdev->dev->attrs.max_qp_init_rd_atom);

  ret = rdma_connect(q->cm_id, &param);//发出一个rdma连接请求
  if (ret) {
    pr_err("rdma_connect failed (%d)\n", ret);
    sswap_rdma_destroy_queue_ib(q);
  }

  return 0;
}

static int sswap_rdma_conn_established(struct rdma_queue *q)
{
  pr_info("connection established\n");
  return 0;
}

static int sswap_rdma_cm_handler(struct rdma_cm_id *cm_id,
    struct rdma_cm_event *ev)
{
  struct rdma_queue *queue = cm_id->context;
  int cm_error = 0;

  pr_info("cm_handler msg: %s (%d) status %d id %p\n", rdma_event_msg(ev->event),
    ev->event, ev->status, cm_id);

  switch (ev->event) {
  case RDMA_CM_EVENT_ADDR_RESOLVED:
    cm_error = sswap_rdma_addr_resolved(queue); //rdma_resolve_route
    break;
  case RDMA_CM_EVENT_ROUTE_RESOLVED:
    cm_error = sswap_rdma_route_resolved(queue, &ev->param.conn); //rdma_connect
    break;
  case RDMA_CM_EVENT_ESTABLISHED:
    queue->cm_error = sswap_rdma_conn_established(queue);//pr_info
    /* complete cm_done regardless of success/failure */
    complete(&queue->cm_done);//完成建链
    return 0;
  case RDMA_CM_EVENT_REJECTED:
    pr_err("connection rejected\n");
    break;
  case RDMA_CM_EVENT_ADDR_ERROR:
  case RDMA_CM_EVENT_ROUTE_ERROR:
  case RDMA_CM_EVENT_CONNECT_ERROR:
  case RDMA_CM_EVENT_UNREACHABLE:
    pr_err("CM error event %d\n", ev->event);
    cm_error = -ECONNRESET;
    break;
  case RDMA_CM_EVENT_DISCONNECTED:
  case RDMA_CM_EVENT_ADDR_CHANGE:
  case RDMA_CM_EVENT_TIMEWAIT_EXIT:
    pr_err("CM connection closed %d\n", ev->event);
    break;
  case RDMA_CM_EVENT_DEVICE_REMOVAL:
    /* device removal is handled via the ib_client API */
    break;
  default:
    pr_err("CM unexpected event: %d\n", ev->event);
    break;
  }

  if (cm_error) {
    queue->cm_error = cm_error;
    complete(&queue->cm_done);
  }

  return 0;
}

inline static int sswap_rdma_wait_for_cm(struct rdma_queue *queue)
{
  wait_for_completion_interruptible_timeout(&queue->cm_done,
    msecs_to_jiffies(CONNECTION_TIMEOUT_MS) + 1);
  return queue->cm_error;
}

static int sswap_rdma_init_queue(struct sswap_rdma_ctrl *ctrl,//rdma_create_id(sswap_rdma_cm_handler) + rdma_resolve_addr
    int idx)
{
  struct rdma_queue *queue;
  int ret;

  pr_info("start: %s\n", __FUNCTION__);

  queue = &ctrl->queues[idx];
  queue->ctrl = ctrl;
  init_completion(&queue->cm_done);
  atomic_set(&queue->pending, 0);
  spin_lock_init(&queue->cq_lock);
  queue->qp_type = get_queue_type(idx);

  queue->cm_id = rdma_create_id(&init_net, sswap_rdma_cm_handler, queue,
      RDMA_PS_TCP, IB_QPT_RC);
  if (IS_ERR(queue->cm_id)) {
    pr_err("failed to create cm id: %ld\n", PTR_ERR(queue->cm_id));
    return -ENODEV;
  }

  queue->cm_error = -ETIMEDOUT;

  //在sswap_rdma_create_ctrl中配置 server和client ip地址和port且进行了rdma_resolve_addr
  ret = rdma_resolve_addr(queue->cm_id, &ctrl->srcaddr, &ctrl->addr,
      CONNECTION_TIMEOUT_MS);
  if (ret) {
    pr_err("rdma_resolve_addr failed: %d\n", ret);
    goto out_destroy_cm_id;
  }

  ret = sswap_rdma_wait_for_cm(queue);//等待sswap_rdma_cm_handler()中  case: RDMA_CM_EVENT_ESTABLISHED queue->cm_done的complete
  if (ret) {
    pr_err("sswap_rdma_wait_for_cm failed\n");
    goto out_destroy_cm_id;
  }

  return 0;

out_destroy_cm_id:
  rdma_destroy_id(queue->cm_id);
  return ret;
}

static void sswap_rdma_stop_queue(struct rdma_queue *q)
{
  rdma_disconnect(q->cm_id);
}

static void sswap_rdma_free_queue(struct rdma_queue *q)
{
  rdma_destroy_qp(q->cm_id);
  ib_free_cq(q->cq);
  rdma_destroy_id(q->cm_id);
}

static int sswap_rdma_init_queues(struct sswap_rdma_ctrl *ctrl)//rdma_resolve_addr
{
  int ret, i;
  for (i = 0; i < numqueues; ++i) {//创建queue并配置queue对应的连接
    ret = sswap_rdma_init_queue(ctrl, i);
    if (ret) {
      pr_err("failed to initialized queue: %d\n", i);
      goto out_free_queues;
    }
  }

  return 0;

out_free_queues:
  for (i--; i >= 0; i--) {
    sswap_rdma_stop_queue(&ctrl->queues[i]);
    sswap_rdma_free_queue(&ctrl->queues[i]);
  }

  return ret;
}

static void sswap_rdma_stopandfree_queues(struct sswap_rdma_ctrl *ctrl)
{
  int i;
  for (i = 0; i < numqueues; ++i) {
    sswap_rdma_stop_queue(&ctrl->queues[i]);
    sswap_rdma_free_queue(&ctrl->queues[i]);
  }
}

static int sswap_rdma_parse_ipaddr(struct sockaddr_in *saddr, char *ip)
{
  u8 *addr = (u8 *)&saddr->sin_addr.s_addr;
  size_t buflen = strlen(ip);

  pr_info("start: %s\n", __FUNCTION__);

  if (buflen > INET_ADDRSTRLEN)
    return -EINVAL;
  if (in4_pton(ip, buflen, addr, '\0', NULL) == 0)//ip地址转换成网络格式 赋值给saddr->sin_addr.s_addr
    return -EINVAL;
  saddr->sin_family = AF_INET;//ip协议
  return 0;
}

static int sswap_rdma_create_ctrl(struct sswap_rdma_ctrl **c)//rdma_resolve_addr
{
  int ret;
  struct sswap_rdma_ctrl *ctrl;
  pr_info("will try to connect to %s:%d\n", serverip, serverport);

  *c = kzalloc(sizeof(struct sswap_rdma_ctrl), GFP_KERNEL);
  if (!*c) {
    pr_err("no mem for ctrl\n");
    return -ENOMEM;
  }
  ctrl = *c;

  ctrl->queues = kzalloc(sizeof(struct rdma_queue) * numqueues, GFP_KERNEL);
  ret = sswap_rdma_parse_ipaddr(&(ctrl->addr_in), serverip);//server ip地址
  if (ret) {
    pr_err("sswap_rdma_parse_ipaddr failed: %d\n", ret);
    return -EINVAL;
  }
  ctrl->addr_in.sin_port = cpu_to_be16(serverport);//server 端口

  ret = sswap_rdma_parse_ipaddr(&(ctrl->srcaddr_in), clientip);//client ip地址
  if (ret) {
    pr_err("sswap_rdma_parse_ipaddr failed: %d\n", ret);
    return -EINVAL;
  }
  /* no need to set the port on the srcaddr */

  return sswap_rdma_init_queues(ctrl);//rdma_resolve_addr
}

static void __exit sswap_rdma_cleanup_module(void)
{
  sswap_rdma_stopandfree_queues(gctrl);
  ib_unregister_client(&sswap_rdma_ib_client);
  kfree(gctrl);
  gctrl = NULL;
  if (req_cache) {
    kmem_cache_destroy(req_cache);
  }
}

static void sswap_rdma_write_done(struct ib_cq *cq, struct ib_wc *wc)
{
  struct rdma_req *req =
    container_of(wc->wr_cqe, struct rdma_req, cqe);
  struct rdma_queue *q = cq->cq_context;
  struct ib_device *ibdev = q->ctrl->rdev->dev;

  if (unlikely(wc->status != IB_WC_SUCCESS)) {
    pr_err("sswap_rdma_write_done status is not success, it is=%d\n", wc->status);
    //q->write_error = wc->status;
  }
  ib_dma_unmap_page(ibdev, req->dma, PAGE_SIZE, DMA_TO_DEVICE);

  atomic_dec(&q->pending);
  kmem_cache_free(req_cache, req);
}

static void sswap_rdma_write_buf_done(struct ib_cq *cq, struct ib_wc *wc)
{
  struct rdma_req *req =
    container_of(wc->wr_cqe, struct rdma_req, cqe);
  struct rdma_queue *q = cq->cq_context;
  struct ib_device *ibdev = q->ctrl->rdev->dev;

  if (unlikely(wc->status != IB_WC_SUCCESS)) {
    pr_err("sswap_rdma_write_done status is not success, it is=%d\n", wc->status);
    //q->write_error = wc->status;
  }
  // ib_dma_unmap_page(ibdev, req->dma, PAGE_SIZE, DMA_TO_DEVICE);
  ib_dma_unmap_single(ibdev, req->dma, req->len, DMA_TO_DEVICE);


  complete(&req->done);//++++ 保证写完成再读
  atomic_dec(&q->pending);
  kmem_cache_free(req_cache, req);
}

static void sswap_rdma_read_done(struct ib_cq *cq, struct ib_wc *wc)
{
  struct rdma_req *req =
    container_of(wc->wr_cqe, struct rdma_req, cqe);
  struct rdma_queue *q = cq->cq_context;
  struct ib_device *ibdev = q->ctrl->rdev->dev;

  if (unlikely(wc->status != IB_WC_SUCCESS))
    pr_err("sswap_rdma_read_done status is not success, it is=%d\n", wc->status);

  ib_dma_unmap_page(ibdev, req->dma, PAGE_SIZE, DMA_FROM_DEVICE);

  SetPageUptodate(req->page);
  unlock_page(req->page);
  complete(&req->done);//没有wait函数
  atomic_dec(&q->pending);
  kmem_cache_free(req_cache, req);
}

static void sswap_rdma_read_buf_done(struct ib_cq *cq, struct ib_wc *wc)
{
  struct rdma_req *req =
    container_of(wc->wr_cqe, struct rdma_req, cqe);
  struct rdma_queue *q = cq->cq_context;
  struct ib_device *ibdev = q->ctrl->rdev->dev;

  if (unlikely(wc->status != IB_WC_SUCCESS))
    pr_err("sswap_rdma_read_done status is not success, it is=%d\n", wc->status);

  ib_dma_unmap_single(ibdev, req->dma, req->len, DMA_FROM_DEVICE);

  // SetPageUptodate(req->page);
  // unlock_page(req->page);
  complete(&req->done);//先完成再释放
  atomic_dec(&q->pending);
  kmem_cache_free(req_cache, req);
}

inline static int sswap_rdma_post_rdma(struct rdma_queue *q, struct rdma_req *qe,
  struct ib_sge *sge, u64 roffset, enum ib_wr_opcode op)
{
  const struct ib_send_wr *bad_wr;
  struct ib_rdma_wr rdma_wr = {};
  int ret;

  BUG_ON(qe->dma == 0);

  sge->addr = qe->dma;
  // sge->length = PAGE_SIZE;
  sge->length = qe->len;//按照创建rdma_req设置sge长度
  sge->lkey = q->ctrl->rdev->pd->local_dma_lkey;

  /* TODO: add a chain of WR, we already have a list so should be easy
   * to just post requests in batches */
  rdma_wr.wr.next    = NULL;
  rdma_wr.wr.wr_cqe  = &qe->cqe;
  rdma_wr.wr.sg_list = sge;
  rdma_wr.wr.num_sge = 1;
  rdma_wr.wr.opcode  = op;
  rdma_wr.wr.send_flags = IB_SEND_SIGNALED;
  rdma_wr.remote_addr = q->ctrl->servermr.baseaddr + roffset;
  rdma_wr.rkey = q->ctrl->servermr.key;

  atomic_inc(&q->pending);
  ret = ib_post_send(q->qp, &rdma_wr.wr, &bad_wr);
  if (unlikely(ret)) {
    pr_err("ib_post_send failed: %d\n", ret);
  }

  return ret;
}

static void sswap_rdma_recv_remotemr_done(struct ib_cq *cq, struct ib_wc *wc)
{
  struct rdma_req *qe =
    container_of(wc->wr_cqe, struct rdma_req, cqe);
  struct rdma_queue *q = cq->cq_context;
  struct sswap_rdma_ctrl *ctrl = q->ctrl;
  struct ib_device *ibdev = q->ctrl->rdev->dev;

  if (unlikely(wc->status != IB_WC_SUCCESS)) {
    pr_err("sswap_rdma_recv_done status is not success\n");
    return;
  }
  ib_dma_unmap_single(ibdev, qe->dma, sizeof(struct sswap_rdma_memregion),
		      DMA_FROM_DEVICE);
  pr_info("servermr baseaddr=%llx, key=%u\n", ctrl->servermr.baseaddr,
	  ctrl->servermr.key);
  complete_all(&qe->done);
}

static int sswap_rdma_post_recv(struct rdma_queue *q, struct rdma_req *qe,///post_recv缓存大小即是bufsize
  size_t bufsize)
{
  const struct ib_recv_wr *bad_wr;
  struct ib_recv_wr wr = {};
  struct ib_sge sge;
  int ret;

  sge.addr = qe->dma;
  sge.length = bufsize;
  sge.lkey = q->ctrl->rdev->pd->local_dma_lkey;

  wr.next    = NULL;
  wr.wr_cqe  = &qe->cqe;
  wr.sg_list = &sge;
  wr.num_sge = 1;

  ret = ib_post_recv(q->qp, &wr, &bad_wr);
  if (ret) {
    pr_err("ib_post_recv failed: %d\n", ret);
  }
  return ret;
}

/* allocates a sswap rdma request, creates a dma mapping for it in
 * req->dma, and synchronizes the dma mapping in the direction of
 * the dma map.
 * Don't touch the page with cpu after creating the request for it!
 * Deallocates the request if there was an error */
inline static int get_req_for_page(struct rdma_req **req, struct ib_device *dev,
				struct page *page, enum dma_data_direction dir)
{
  int ret;

  ret = 0;
  *req = kmem_cache_alloc(req_cache, GFP_ATOMIC);
  if (unlikely(!req)) {
    pr_err("no memory for req\n");
    ret = -ENOMEM;
    goto out;
  }

  (*req)->page = page;
  init_completion(&(*req)->done);//done时没有释放 可能是因为 write时 不需要阻塞等待 write完成 读存在wait

  (*req)->dma = ib_dma_map_page(dev, page, 0, PAGE_SIZE, dir);
  if (unlikely(ib_dma_mapping_error(dev, (*req)->dma))) {
    pr_err("ib_dma_mapping_error\n");
    ret = -ENOMEM;
    kmem_cache_free(req_cache, req);
    goto out;
  }

  ib_dma_sync_single_for_device(dev, (*req)->dma, PAGE_SIZE, dir);
out:
  return ret;
}

/* the buffer needs to come from kernel (not high memory) */
inline static int get_req_for_buf(struct rdma_req **req, struct ib_device *dev,
				void *buf, size_t size,
				enum dma_data_direction dir)
{
  int ret;

  ret = 0;
  *req = kmem_cache_alloc(req_cache, GFP_ATOMIC);//在done中释放 kmem_cache_free(req_cache, req);
  if (unlikely(!req)) {
    pr_err("no memory for req\n");
    ret = -ENOMEM;
    goto out;
  }

  init_completion(&(*req)->done);//等待完成 recv_mr时complete_all(&qe->done);

  (*req)->dma = ib_dma_map_single(dev, buf, size, dir);
  if (unlikely(ib_dma_mapping_error(dev, (*req)->dma))) {
    pr_err("ib_dma_mapping_error\n");
    ret = -ENOMEM;
    kmem_cache_free(req_cache, req);
    goto out;
  }

  ib_dma_sync_single_for_device(dev, (*req)->dma, size, dir);
out:
  return ret;
}

inline static void sswap_rdma_wait_completion(struct ib_cq *cq,
					      struct rdma_req *qe)
{
  ndelay(1000);//ns
  while (!completion_done(&qe->done)) {
    ndelay(250);
    ib_process_cq_direct(cq, 1);
  }
}

/* polls queue until we reach target completed wrs or qp is empty */
static inline int poll_target(struct rdma_queue *q, int target)
{
  unsigned long flags;
  int completed = 0;

  while (completed < target && atomic_read(&q->pending) > 0) {
    spin_lock_irqsave(&q->cq_lock, flags);
    completed += ib_process_cq_direct(q->cq, target - completed);
    spin_unlock_irqrestore(&q->cq_lock, flags);
    cpu_relax();
  }

  return completed;
}

static inline int drain_queue(struct rdma_queue *q)
{
  unsigned long flags;

  while (atomic_read(&q->pending) > 0) {//每次执行完成done都会 atomic_dec(&q->pending); 每次post rdma请求都会atomic_inc(&q->pending)
    spin_lock_irqsave(&q->cq_lock, flags);
    ib_process_cq_direct(q->cq, 16);//处理所有未完成的CQ entry
    spin_unlock_irqrestore(&q->cq_lock, flags);
    cpu_relax();
  }

  return 1;
}

static inline int write_queue_add(struct rdma_queue *q, struct page *page,
				  u64 roffset)
{
  struct rdma_req *req;
  struct ib_device *dev = q->ctrl->rdev->dev;
  struct ib_sge sge = {};
  int ret, inflight;

  while ((inflight = atomic_read(&q->pending)) >= QP_MAX_SEND_WR - 8) {
    BUG_ON(inflight > QP_MAX_SEND_WR);
    poll_target(q, 2048);
    pr_info_ratelimited("back pressure writes");
  }

  ret = get_req_for_page(&req, dev, page, DMA_TO_DEVICE);
  if (unlikely(ret))
    return ret;

  req->cqe.done = sswap_rdma_write_done;
  ret = sswap_rdma_post_rdma(q, req, &sge, roffset, IB_WR_RDMA_WRITE);

  return ret;
}

static inline int begin_read(struct rdma_queue *q, struct page *page,
			     u64 roffset)
{
  struct rdma_req *req;
  struct ib_device *dev = q->ctrl->rdev->dev;
  struct ib_sge sge = {};
  int ret, inflight;

  /* back pressure in-flight reads, can't send more than
   * QP_MAX_SEND_WR at a time */
  while ((inflight = atomic_read(&q->pending)) >= QP_MAX_SEND_WR) {
    BUG_ON(inflight > QP_MAX_SEND_WR); /* only valid case is == */
    poll_target(q, 8);
    pr_info_ratelimited("back pressure happened on reads");
  }

  ret = get_req_for_page(&req, dev, page, DMA_TO_DEVICE);
  if (unlikely(ret))
    return ret;

  req->cqe.done = sswap_rdma_read_done;
  ret = sswap_rdma_post_rdma(q, req, &sge, roffset, IB_WR_RDMA_READ);
  //这里没有wait等待req->done的函数 直接返回 在recv mr中有
  return ret;
}

int sswap_rdma_write(struct page *page, u64 roffset)
{
  int ret;
  struct rdma_queue *q;

  VM_BUG_ON_PAGE(!PageSwapCache(page), page);

  q = sswap_rdma_get_queue(smp_processor_id(), QP_WRITE_SYNC);
  ret = write_queue_add(q, page, roffset);
  BUG_ON(ret);
  drain_queue(q);
  return ret;
}
EXPORT_SYMBOL(sswap_rdma_write);

static int sswap_rdma_recv_remotemr(struct sswap_rdma_ctrl *ctrl)
{
  struct rdma_req *qe;
  int ret;
  struct ib_device *dev;

  pr_info("start: %s\n", __FUNCTION__);
  dev = ctrl->rdev->dev;

  ret = get_req_for_buf(&qe, dev, &(ctrl->servermr), sizeof(ctrl->servermr),
			DMA_FROM_DEVICE);
  if (unlikely(ret))
    goto out;

  qe->cqe.done = sswap_rdma_recv_remotemr_done;

  ret = sswap_rdma_post_recv(&(ctrl->queues[0]), qe, sizeof(struct sswap_rdma_memregion));

  if (unlikely(ret))
    goto out_free_qe;

  /* this delay doesn't really matter, only happens once */
  sswap_rdma_wait_completion(ctrl->queues[0].cq, qe);
  //必须等待sswap_rdma_recv_remotemr_done完成才能返回 对于fastswap读写page请求不需要wait post请求直接 done时才unlock page

out_free_qe:
  kmem_cache_free(req_cache, qe);
out:
  return ret;
}

/* page is unlocked when the wr is done.
 * posts an RDMA read on this cpu's qp */
int sswap_rdma_read_async(struct page *page, u64 roffset)
{
  struct rdma_queue *q;
  int ret;

  VM_BUG_ON_PAGE(!PageSwapCache(page), page);
  VM_BUG_ON_PAGE(!PageLocked(page), page);
  VM_BUG_ON_PAGE(PageUptodate(page), page);

  q = sswap_rdma_get_queue(smp_processor_id(), QP_READ_ASYNC);
  ret = begin_read(q, page, roffset);
  return ret;
}
EXPORT_SYMBOL(sswap_rdma_read_async);

int sswap_rdma_read_sync(struct page *page, u64 roffset)
{
  struct rdma_queue *q;
  int ret;

  VM_BUG_ON_PAGE(!PageSwapCache(page), page);
  VM_BUG_ON_PAGE(!PageLocked(page), page);
  VM_BUG_ON_PAGE(PageUptodate(page), page);

  q = sswap_rdma_get_queue(smp_processor_id(), QP_READ_SYNC);
  ret = begin_read(q, page, roffset);
  return ret;
}
EXPORT_SYMBOL(sswap_rdma_read_sync);

int sswap_rdma_poll_load(int cpu)
{
  struct rdma_queue *q = sswap_rdma_get_queue(cpu, QP_READ_SYNC);
  return drain_queue(q);
}
EXPORT_SYMBOL(sswap_rdma_poll_load);

/* idx is absolute id (i.e. > than number of cpus) */
inline enum qp_type get_queue_type(unsigned int idx)
{
  // numcpus = 8
  if (idx < numcpus)
    return QP_READ_SYNC;
  else if (idx < numcpus * 2)
    return QP_READ_ASYNC;
  else if (idx < numcpus * 3)
    return QP_WRITE_SYNC;

  BUG();
  return QP_READ_SYNC;
}

inline struct rdma_queue *sswap_rdma_get_queue(unsigned int cpuid,
					       enum qp_type type)
{
  BUG_ON(gctrl == NULL);

  switch (type) {
    case QP_READ_SYNC:
      return &gctrl->queues[cpuid];
    case QP_READ_ASYNC:
      return &gctrl->queues[cpuid + numcpus];
    case QP_WRITE_SYNC:
      return &gctrl->queues[cpuid + numcpus * 2];
    default:
      BUG();
  };
}

// static void compress_test(void){
//   void *src, *dst, *buf;
//   int ret;
//   u16 crc_w, crc_r;
//   int dlen, slen;
//   int buflen;
//   int compress_ret;

//   buflen = 4096;
  
//   // src -[cp]-> buf -[dcp]-> dst
//   // buflen --> dlen --> slen
//   pr_info("*********** begin compress test *********");

//   src = kmalloc(2 * buflen, GFP_KERNEL);
//   dst = kmalloc(2 * buflen, GFP_KERNEL);
//   buf = kmalloc(2 * buflen, GFP_KERNEL);
//   // get_random_bytes(src, buflen);
//   memset(src, 7, buflen);

//   crc_w = crc16(0x0000, src, buflen);


//   compress(src, buflen, buf, &dlen);// src -> buf | buflen -> dlen

//   pr_info("crc: %hx compress len: %d", crc_w, dlen);

//   // ndelay(1000);

//   compress_ret = decompress(buf, dlen, dst, &slen);//buf -> dst | dlen -> slen

//   // if(slen != buflen){
//   //   goto out;
//   // }
//   crc_r = crc16(0x0000, dst, slen);

//   pr_info("crc: %hx decompress len: %d ret: %d", crc_r, slen, compress_ret);

//   kfree(src);
//   kfree(dst);
//   kfree(buf);

// out:
//   return;

// }//tmd 怎么跑都搞不通

static void compress_test(void){
  	u16 crc_w, crc_r;
	// u16 a = 0x0000;
	// u8 b = 0x13;
	void *src, *dst, *buf;

    int slen;
	int dlen;
    int buflen = PAGE_SIZE;

	// get_random_bytes(&a, sizeof(int));
	// pr_info("random number: %d", a);

  pr_info("*********** begin compress test *********");

	src = (u8 *)kmalloc(2 * buflen, GFP_KERNEL);//
	dst = (u8 *)kmalloc(2 * buflen, GFP_KERNEL);//
	buf = (u8 *)kmalloc(2 * buflen, GFP_KERNEL);
	// get_random_bytes(src, PAGE_SIZE);//
	memset(src, 7, buflen);

	crc_w = crc16(0x0000, src, buflen);
	

	compress(src, buflen, buf, &dlen);//默认page_size  

	pr_info("first crc: %d compress len: %u", crc_w, dlen);
    

	
	decompress(buf, dlen, dst, &slen);// src -> buf -> dst | buflen -> dlen -> slen

  crc_r = crc16(0x0000, dst, slen);
    


	// pr_info("random number: %d", a);
	pr_info("second crc: %d decompress len: %u", crc_r, slen);
	
	// src = (u8 *)kmalloc(2, GFP_KERNEL);
	

	
	
	// crc_ans = crc16_byte(0x0000, 0x13);
 	// pr_info("first crc: %d", crc_ans);
	
	kfree(src); 
	kfree(dst);
	kfree(buf);
	// return 0;
}






static int pingpong_test(void){
  int ret;
  struct rdma_queue *q_read, *q_write;
  struct rdma_req *req_write, *req_read;
  struct ib_device *dev;
  struct ib_sge sge = {};
  int inflight, buflen;
  void *src, *dst;
  char *s = "hello world";
  u64 roffset = 0x0;
  

  pr_info("*********** begin pingpong test *********");

  src = kmalloc(4096, GFP_KERNEL);
  dst = kmalloc(4096, GFP_KERNEL);

  buflen = strlen(s) + 1;
  memcpy(src, s, buflen);


  // q_write = sswap_rdma_get_queue(smp_processor_id(), QP_WRITE_SYNC);
  // q_read = sswap_rdma_get_queue(smp_processor_id(), QP_READ_SYNC);

  q_write = &(gctrl->queues[0]);//直接用第1个和第2个queue
  q_read = &(gctrl->queues[1]);


  //******** 写 **************
  dev = q_write->ctrl->rdev->dev;//dev应该可以共享

  while ((inflight = atomic_read(&q_write->pending)) >= QP_MAX_SEND_WR - 8) {
    BUG_ON(inflight > QP_MAX_SEND_WR);
    poll_target(q_write, 2048);
    pr_info_ratelimited("back pressure writes");
  }

  ret = get_req_for_buf(&req_write, dev, src, buflen, DMA_TO_DEVICE);
  if (unlikely(ret))
    return ret;
  req_write->len = buflen;
  req_write->cqe.done = sswap_rdma_write_buf_done;
  ret = sswap_rdma_post_rdma(q_write, req_write, &sge, roffset, IB_WR_RDMA_WRITE);

  drain_queue(q_write);//处理完所有的write done请求 ib_process_cq_direct(q->cq, 16);

  sswap_rdma_wait_completion(q_write->cq, req_write);//ib_process_cq_direct(cq, 1);

  //******** 读 **************
  dev = q_read->ctrl->rdev->dev;//dev应该可以共享
  while ((inflight = atomic_read(&q_read->pending)) >= QP_MAX_SEND_WR) {
    BUG_ON(inflight > QP_MAX_SEND_WR); /* only valid case is == */
    poll_target(q_read, 8);
    pr_info_ratelimited("back pressure happened on reads");
  }

  ret = get_req_for_buf(&req_read, dev, dst, buflen, DMA_FROM_DEVICE);
  if (unlikely(ret))
    return ret;
  req_read->len = buflen;
  req_read->cqe.done = sswap_rdma_read_buf_done;
  ret = sswap_rdma_post_rdma(q_read, req_read, &sge, roffset, IB_WR_RDMA_READ);
  
  drain_queue(q_read);

  sswap_rdma_wait_completion(q_read->cq, req_read);//等待read_done 完成

  pr_info("%s", (char *)dst);

  kfree(src);
  kfree(dst);

  return ret;
}

static int pingpong_test_compress(void){
  int ret;
  struct rdma_queue *q_read, *q_write;
  struct rdma_req *req_write, *req_read;
  struct ib_device *dev;
  struct ib_sge sge = {};
  int inflight, buflen;
  void *src, *dst, *buf_read, *buf_write;
  u16 crc_write, crc_read;
  u16 crc_w, crc_r;
  int dlen, slen;


  u64 roffset = 0x0;

  buflen = 4096;
  
  // src -[cp]-> buf_write --> buf_read -[dcp]-> dst
  pr_info("*********** begin pingpong compress test *********");

  src = kmalloc(2 * buflen, GFP_KERNEL);
  dst = kmalloc(2 * buflen, GFP_KERNEL);
  buf_write = kmalloc(2 * buflen, GFP_KERNEL);
  buf_read = kmalloc(2 * buflen, GFP_KERNEL);
  

  // get_random_bytes(src, buflen); //设置随机数 会有问题 大部分 都不能压缩 4096 --> 4116
  memset(src, 7, buflen);

  crc_w = crc16(0x0000, src, buflen);
  compress(src, buflen, buf_write, &dlen);// buflen --> dlen
  crc_write = crc16(0x0000, buf_write, dlen);

  // pr_info("random num:");
  // p = (char *)src;
  // for(i = 0; i < buflen; i++){
  //   printk("%02X ", p[i]);
  // }
  // printk("\n");
  pr_info("uncompress len: %d crc: %hx", buflen, crc_w);
  pr_info("write compress len: %d crc: %hx", dlen, crc_write);

  // q_write = sswap_rdma_get_queue(smp_processor_id(), QP_WRITE_SYNC);
  // q_read = sswap_rdma_get_queue(smp_processor_id(), QP_READ_SYNC);
  if(dlen > 4096){//没有压缩 就不写
    goto out;
  }

  q_write = &(gctrl->queues[2]);//直接用第1个和第2个queue
  q_read = &(gctrl->queues[3]);


  //******** 写 **************
  dev = q_write->ctrl->rdev->dev;//dev应该可以共享

  while ((inflight = atomic_read(&q_write->pending)) >= QP_MAX_SEND_WR - 8) {
    BUG_ON(inflight > QP_MAX_SEND_WR);
    poll_target(q_write, 2048);
    pr_info_ratelimited("back pressure writes");
  }

  ret = get_req_for_buf(&req_write, dev, buf_write, dlen, DMA_TO_DEVICE);//在kmem cache中分配rdma_req对象空间 
  if (unlikely(ret))
    return ret;
  req_write->len = dlen;
  req_write->cqe.done = sswap_rdma_write_buf_done;
  ret = sswap_rdma_post_rdma(q_write, req_write, &sge, roffset, IB_WR_RDMA_WRITE);

  drain_queue(q_write);//处理完所有的write done请求 ib_process_cq_direct(q->cq, 16);

  sswap_rdma_wait_completion(q_write->cq, req_write);//ib_process_cq_direct(cq, 1);

  //******** 读 **************
  dev = q_read->ctrl->rdev->dev;//dev应该可以共享
  while ((inflight = atomic_read(&q_read->pending)) >= QP_MAX_SEND_WR) {
    BUG_ON(inflight > QP_MAX_SEND_WR); /* only valid case is == */
    poll_target(q_read, 8);
    pr_info_ratelimited("back pressure happened on reads");
  }

  ret = get_req_for_buf(&req_read, dev, buf_read, dlen, DMA_FROM_DEVICE);
  if (unlikely(ret))
    return ret;
  req_read->len = dlen;
  req_read->cqe.done = sswap_rdma_read_buf_done;
  ret = sswap_rdma_post_rdma(q_read, req_read, &sge, roffset, IB_WR_RDMA_READ);
  
  drain_queue(q_read);

  sswap_rdma_wait_completion(q_read->cq, req_read);//等待read_done 完成

  //******** 验证 **************

  // pr_info("************************");

  crc_read = crc16(0x0000, buf_read, dlen);
  //buflen -> dlen -> slen
  decompress(buf_read, dlen, dst, &slen);

  crc_r = crc16(0x0000, dst, slen);

  // pr_info("crc: %hx", crc_read);

  pr_info("read compress len: %d crc: %hx", dlen, crc_read);

  pr_info("decompress len: %d crc: %hx", slen, crc_r);



  // p = (char *)dst;
  // for(i = 0; i < buflen; i++){
  //   printk("%02X ", p[i]);
  // }
  // printk("\n");

  

out:
  kfree(src);
  kfree(dst);
  kfree(buf_write);
  kfree(buf_read);

  return ret;
}


static int __init sswap_rdma_init_module(void)
{
  int ret;

  pr_info("start: %s\n", __FUNCTION__);
  pr_info("* RDMA BACKEND *");

  // modified by ysjing
  // numcpus = num_online_cpus(
  numqueues = numcpus * 3;

  req_cache = kmem_cache_create("sswap_req_cache", sizeof(struct rdma_req), 0,
                      SLAB_TEMPORARY | SLAB_HWCACHE_ALIGN, NULL);//Align objs on cache line且short-lived

  if (!req_cache) {
    pr_err("no memory for cache allocation\n");
    return -ENOMEM;
  }

  ib_register_client(&sswap_rdma_ib_client);
  ret = sswap_rdma_create_ctrl(&gctrl);//在sswap_rdma_create_ctrl中配置 server和client ip地址和port且进行了rdma_resolve_addr
  if (ret) {
    pr_err("could not create ctrl\n");
    ib_unregister_client(&sswap_rdma_ib_client);
    return -ENODEV;
  }

  ret = sswap_rdma_recv_remotemr(gctrl);
  if (ret) {
    pr_err("could not setup remote memory region\n");
    ib_unregister_client(&sswap_rdma_ib_client);
    return -ENODEV;
  }

  pr_info("ctrl is ready for reqs\n");

  //******** ping_pong测试 **************
  compress_test();
  pingpong_test(); 
  pingpong_test_compress();
  pr_info("ping pong test done");
    


  return 0;
}

module_init(sswap_rdma_init_module);
module_exit(sswap_rdma_cleanup_module);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Experiments");