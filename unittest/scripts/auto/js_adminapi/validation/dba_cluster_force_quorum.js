//@<OUT> Get cluster operation must show a warning because there is no quorum
WARNING: Cluster has no quorum and cannot process write transactions: Group has no quorum

//@ Cluster.forceQuorumUsingPartitionOf errors
||Cluster.forceQuorumUsingPartitionOf: Invalid number of arguments, expected 1 to 2 but got 0
||Cluster.forceQuorumUsingPartitionOf: Argument #1: Invalid connection options, expected either a URI or a Connection Options Dictionary
||Cluster.forceQuorumUsingPartitionOf: Argument #1: Invalid URI: empty.
||Cluster.forceQuorumUsingPartitionOf: Argument #1: Invalid connection options, expected either a URI or a Connection Options Dictionary
||Cluster.forceQuorumUsingPartitionOf: The instance 'localhost:<<<__mysql_sandbox_port2>>>' cannot be used to restore the cluster as it is not an active member of replication group.

//@<OUT> BUG#30739252: Confirm instance 3 is never included as OFFLINE (no undefined behaviour) {VER(>=8.0.16) && VER(<8.0.27)}
CHANNEL_NAME	MEMBER_ID	MEMBER_HOST	MEMBER_PORT	MEMBER_STATE	MEMBER_ROLE	MEMBER_VERSION
group_replication_applier	[[*]]	<<<hostname>>>	<<<__mysql_sandbox_port2>>>	ONLINE	PRIMARY	<<<__version>>>
//@<OUT> BUG#30739252: Confirm instance 3 is never included as OFFLINE (no undefined behaviour) {VER(>=8.0.27)}
CHANNEL_NAME	MEMBER_ID	MEMBER_HOST	MEMBER_PORT	MEMBER_STATE	MEMBER_ROLE	MEMBER_VERSION	MEMBER_COMMUNICATION_STACK
group_replication_applier	[[*]]	<<<hostname>>>	<<<__mysql_sandbox_port2>>>	ONLINE	PRIMARY	<<<__version>>>	MySQL
