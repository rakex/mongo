// btree.cpp

/**
*    Copyright (C) 2008 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "pch.h"
#include "db.h"
#include "btree.h"
#include "pdfile.h"
#include "json.h"
#include "clientcursor.h"
#include "client.h"
#include "dbhelpers.h"
#include "curop-inl.h"
#include "stats/counters.h"

namespace mongo {

#if !defined(_DURABLE) || !defined(_DEBUG)
#define VERIFYTHISLOC dassert( thisLoc.btree() == this );
#else
// with _DURABLE, this assert wouldn't work without getting fancier as there are multiple mmap views for _DEBUG mode...
#define VERIFYTHISLOC 
#endif

    /** give us a writable version of the btree bucket (declares write intent). 
        note it is likely more efficient to declare write intent on something smaller when you can.
    */
    BtreeBucket* DiskLoc::btreemod() const {
        assert( _a != -1 );
        BtreeBucket *b = const_cast< BtreeBucket * >( btree() );
        return static_cast< BtreeBucket* >( dur::writingPtr( b, BucketSize ) );
    }

    _KeyNode& _KeyNode::writing() const { 
        return *dur::writing( const_cast< _KeyNode* >( this ) );
    }

    KeyNode::KeyNode(const BucketBasics& bb, const _KeyNode &k) :
            prevChildBucket(k.prevChildBucket),
            recordLoc(k.recordLoc), key(bb.data+k.keyDataOfs())
    { }

    /* largest key size we allow.  note we very much need to support bigger keys (somehow) in the future. */
    static const int KeyMax = BucketSize / 10;

    static const int split_debug = 0;
    static const int insert_debug = 0;

    extern int otherTraceLevel;

    /* this error is ok/benign when doing a background indexing -- that logic in pdfile checks explicitly 
       for the 10287 error code.
       */
    static void alreadyInIndex() { 
        // we don't use massert() here as that does logging and this is 'benign' - see catches in _indexRecord()
        throw MsgAssertionException(10287, "btree: key+recloc already in index");
    }

    /* BucketBasics --------------------------------------------------- */

    string BtreeBucket::bucketSummary() const {
        stringstream ss;
        ss << "  Bucket info:" << endl;
        ss << "    n: " << n << endl;
        ss << "    parent: " << parent.toString() << endl;
        ss << "    nextChild: " << parent.toString() << endl;
        ss << "    flags:" << flags << endl;
        ss << "    emptySize: " << emptySize << " topSize: " << topSize << endl;
        return ss.str();
    }
        
    int BucketBasics::Size() const {
        assert( _wasSize == BucketSize );
        return BucketSize;
    }
    inline void BucketBasics::setNotPacked() {
        flags &= ~Packed;
    }
    inline void BucketBasics::setPacked() {
        flags |= Packed;
    }

    void BucketBasics::_shape(int level, stringstream& ss) const {
        for ( int i = 0; i < level; i++ ) ss << ' ';
        ss << "*\n";
        for ( int i = 0; i < n; i++ )
            if ( !k(i).prevChildBucket.isNull() )
                k(i).prevChildBucket.btree()->_shape(level+1,ss);
        if ( !nextChild.isNull() )
            nextChild.btree()->_shape(level+1,ss);
    }

    int bt_fv=0;
    int bt_dmp=0;

    void BtreeBucket::dumpTree(const DiskLoc &thisLoc, const BSONObj &order) const {
        bt_dmp=1;
        fullValidate(thisLoc, order);
        bt_dmp=0;
    }

    int BtreeBucket::fullValidate(const DiskLoc& thisLoc, const BSONObj &order, int *unusedCount) const {
        {
            bool f = false;
            assert( f = true );
            massert( 10281 , "assert is misdefined", f);
        }

        killCurrentOp.checkForInterrupt();
        assertValid(order, true);
//	if( bt_fv==0 )
//		return;

        if ( bt_dmp ) {
            out() << thisLoc.toString() << ' ';
            ((BtreeBucket *) this)->dump();
        }

        // keycount
        int kc = 0;

        for ( int i = 0; i < n; i++ ) {
            const _KeyNode& kn = k(i);

            if ( kn.isUsed() ) {
                kc++;
            } else {
                if ( unusedCount ) {
                    ++( *unusedCount );
                }
            }
            if ( !kn.prevChildBucket.isNull() ) {
                DiskLoc left = kn.prevChildBucket;
                const BtreeBucket *b = left.btree();
                wassert( b->parent == thisLoc );
                kc += b->fullValidate(kn.prevChildBucket, order, unusedCount);
            }
        }
        if ( !nextChild.isNull() ) {
            const BtreeBucket *b = nextChild.btree();
            wassert( b->parent == thisLoc );
            kc += b->fullValidate(nextChild, order, unusedCount);
        }

        return kc;
    }

    int nDumped = 0;

    void BucketBasics::assertValid(const Ordering &order, bool force) const {
        if ( !debug && !force )
            return;
        wassert( n >= 0 && n < Size() );
        wassert( emptySize >= 0 && emptySize < BucketSize );
        wassert( topSize >= n && topSize <= BucketSize );

        // this is very slow so don't do often
        {
            static int _k;
            if( ++_k % 128 ) 
                return;
        }

        DEV {
            // slow:
            for ( int i = 0; i < n-1; i++ ) {
                BSONObj k1 = keyNode(i).key;
                BSONObj k2 = keyNode(i+1).key;
                int z = k1.woCompare(k2, order); //OK
                if ( z > 0 ) {
                    out() << "ERROR: btree key order corrupt.  Keys:" << endl;
                    if ( ++nDumped < 5 ) {
                        for ( int j = 0; j < n; j++ ) {
                            out() << "  " << keyNode(j).key.toString() << endl;
                        }
                        ((BtreeBucket *) this)->dump();
                    }
                    wassert(false);
                    break;
                }
                else if ( z == 0 ) {
                    if ( !(k(i).recordLoc < k(i+1).recordLoc) ) {
                        out() << "ERROR: btree key order corrupt (recordloc's wrong).  Keys:" << endl;
                        out() << " k(" << i << "):" << keyNode(i).key.toString() << " RL:" << k(i).recordLoc.toString() << endl;
                        out() << " k(" << i+1 << "):" << keyNode(i+1).key.toString() << " RL:" << k(i+1).recordLoc.toString() << endl;
                        wassert( k(i).recordLoc < k(i+1).recordLoc );
                    }
                }
            }
        }
        else {
            //faster:
            if ( n > 1 ) {
                BSONObj k1 = keyNode(0).key;
                BSONObj k2 = keyNode(n-1).key;
                int z = k1.woCompare(k2, order);
                //wassert( z <= 0 );
                if ( z > 0 ) {
                    problem() << "btree keys out of order" << '\n';
                    ONCE {
                        ((BtreeBucket *) this)->dump();
                    }
                    assert(false);
                }
            }
        }
    }

    inline void BucketBasics::markUnused(int keypos) {
        assert( keypos >= 0 && keypos < n );
        k(keypos).setUnused();
    }

    inline int BucketBasics::totalDataSize() const {
        return (int) (Size() - (data-(char*)this));
    }

    void BucketBasics::init() {
        parent.Null();
        nextChild.Null();
        _wasSize = BucketSize;
        _reserved1 = 0;
        flags = Packed;
        n = 0;
        emptySize = totalDataSize();
        topSize = 0;
        reserved = 0;
    }

    /* see _alloc */
    inline void BucketBasics::_unalloc(int bytes) {
        topSize -= bytes;
        emptySize += bytes;
    }

    /* we allocate space from the end of the buffer for data.
       the keynodes grow from the front.
    */
    inline int BucketBasics::_alloc(int bytes) {
        topSize += bytes;
        emptySize -= bytes;
        int ofs = totalDataSize() - topSize;
        assert( ofs > 0 );
        return ofs;
    }

    void BucketBasics::_delKeyAtPos(int keypos, bool mayEmpty) {
        assert( keypos >= 0 && keypos <= n );
        assert( childForPos(keypos).isNull() );
        // TODO audit cases where nextChild is null
        assert( ( mayEmpty && n > 0 ) || n > 1 || nextChild.isNull() );
        emptySize += sizeof(_KeyNode);
        n--;
        for ( int j = keypos; j < n; j++ )
            k(j) = k(j+1);
        setNotPacked();
    }

    /* pull rightmost key from the bucket.  this version requires its right child to be null so it 
	   does not bother returning that value.
    */
    void BucketBasics::popBack(DiskLoc& recLoc, BSONObj& key) { 
        massert( 10282 ,  "n==0 in btree popBack()", n > 0 );
        assert( k(n-1).isUsed() ); // no unused skipping in this function at this point - btreebuilder doesn't require that
        KeyNode kn = keyNode(n-1);
        recLoc = kn.recordLoc;
        key = kn.key;
        int keysize = kn.key.objsize();

		massert( 10283 , "rchild not null in btree popBack()", nextChild.isNull());

		/* weirdly, we also put the rightmost down pointer in nextchild, even when bucket isn't full. */
		nextChild = kn.prevChildBucket;

        n--;
        emptySize += sizeof(_KeyNode);
        _unalloc(keysize);
    }

    /* add a key.  must be > all existing.  be careful to set next ptr right. */
    bool BucketBasics::_pushBack(const DiskLoc recordLoc, const BSONObj& key, const Ordering &order, const DiskLoc prevChild) {
        int bytesNeeded = key.objsize() + sizeof(_KeyNode);
        if ( bytesNeeded > emptySize )
            return false;
        assert( bytesNeeded <= emptySize );
        assert( n == 0 || keyNode(n-1).key.woCompare(key, order) <= 0 );
        emptySize -= sizeof(_KeyNode);
        _KeyNode& kn = k(n++);
        kn.prevChildBucket = prevChild;
        kn.recordLoc = recordLoc;
        kn.setKeyDataOfs( (short) _alloc(key.objsize()) );
        char *p = dataAt(kn.keyDataOfs());
        memcpy(p, key.objdata(), key.objsize());
        return true;
    }
    /*void BucketBasics::pushBack(const DiskLoc& recordLoc, BSONObj& key, const BSONObj &order, DiskLoc prevChild, DiskLoc nextChild) { 
        pushBack(recordLoc, key, order, prevChild);
        childForPos(n) = nextChild;
    }*/

    /* insert a key in a bucket with no complexity -- no splits required */
    bool BucketBasics::basicInsert(const DiskLoc thisLoc, int &keypos, const DiskLoc recordLoc, const BSONObj& key, const Ordering &order) {
        assert( keypos >= 0 && keypos <= n );
        int bytesNeeded = key.objsize() + sizeof(_KeyNode);
        if ( bytesNeeded > emptySize ) {
            pack( order, keypos );
            if ( bytesNeeded > emptySize )
                return false;
        }
        for ( int j = n; j > keypos; j-- ) // make room
            k(j) = k(j-1);

        BucketBasics *b = this;

        b->n++;
        b->emptySize -= sizeof(_KeyNode);
        _KeyNode& kn = b->k(keypos);
        kn.prevChildBucket.Null();
        kn.recordLoc = recordLoc;
        kn.setKeyDataOfs((short) b->_alloc(key.objsize()) );
        char *p = b->dataAt(kn.keyDataOfs());
        memcpy(p, key.objdata(), key.objsize());
        return true;
    }

    // with this implementation, refPos == 0 disregards effect of refPos
    bool BucketBasics::mayDropKey( int index, int refPos ) const {
        return index > 0 && ( index != refPos ) && k( index ).isUnused() && k( index ).prevChildBucket.isNull();
    }
    
    int BucketBasics::packedDataSize( int refPos ) const {
        if ( flags & Packed ) {
            return BucketSize - emptySize - headerSize();
        }
        int size = 0;
        for( int j = 0; j < n; ++j ) {
            if ( mayDropKey( j, refPos ) ) {
                continue;
            }
            size += keyNode( j ).key.objsize() + sizeof( _KeyNode );
        }
        return size;
    }
    
    /* when we delete things we just leave empty space until the node is
       full and then we repack it.
    */
    void BucketBasics::pack( const Ordering &order, int &refPos ) {
        if ( flags & Packed )
            return;

        int tdz = totalDataSize();
        char temp[BucketSize];
        int ofs = tdz;
        topSize = 0;
        int i = 0;
        for ( int j = 0; j < n; j++ ) {
            if( mayDropKey( j, refPos ) ) {
                continue; // key is unused and has no children - drop it
            }
            if( i != j ) {
                if ( refPos == j ) {
                    refPos = i; // i < j so j will never be refPos again
                }
                k( i ) = k( j );
            }
            short ofsold = k(i).keyDataOfs();
            int sz = keyNode(i).key.objsize();
            ofs -= sz;
            topSize += sz;
            memcpy(temp+ofs, dataAt(ofsold), sz);
            k(i).setKeyDataOfsSavingUse( ofs );
            ++i;
        }
        if ( refPos == n ) {
            refPos = i;
        }
        n = i;
        int dataUsed = tdz - ofs;
        memcpy(data + ofs, temp + ofs, dataUsed);
        emptySize = tdz - dataUsed - n * sizeof(_KeyNode);
        assert( emptySize >= 0 );

        setPacked();
        
        assertValid( order );
    }

    inline void BucketBasics::truncateTo(int N, const Ordering &order, int &refPos) {
        n = N;
        setNotPacked();
        pack( order, refPos );
    }

    /**
     * In the standard btree algorithm, we would split based on the
     * existing keys _and_ the new key.  But that's more work to
     * implement, so we split the existing keys and then add the new key.
     */
    int BucketBasics::splitPos( int keypos ) const {
        assert( n > 2 );
        int split = 0;
        int rightSize = 0;
        // when splitting a btree node, if the new key is greater than all the other keys, we should not do an even split, but a 90/10 split. 
        // see SERVER-983
        int rightSizeLimit = topSize / ( keypos == n ? 10 : 2 );
        for( int i = n - 1; i > -1; --i ) {
            rightSize += keyNode( i ).key.objsize();
            if ( rightSize > rightSizeLimit ) {
                split = i;
                break;
            }
        }
        if ( split < 1 ) {
            split = 1;
        } else if ( split > n - 2 ) {
            split = n - 2;
        }
        
        return split;
    }
        
    /* - BtreeBucket --------------------------------------------------- */

    /* return largest key in the subtree. */
    void BtreeBucket::findLargestKey(const DiskLoc& thisLoc, DiskLoc& largestLoc, int& largestKey) {
        DiskLoc loc = thisLoc;
        while ( 1 ) {
            const BtreeBucket *b = loc.btree();
            if ( !b->nextChild.isNull() ) {
                loc = b->nextChild;
                continue;
            }

            assert(b->n>0);
            largestLoc = loc;
            largestKey = b->n-1;

            break;
        }
    }
    
    /**
     * NOTE Currently the Ordering implementation assumes a compound index will
     * not have more keys than an unsigned variable has bits.  The same
     * assumption is used in the implementation below with respect to the 'mask'
     * variable.
     */
    int BtreeBucket::customBSONCmp( const BSONObj &l, const BSONObj &rBegin, int rBeginLen, bool rSup, const vector< const BSONElement * > &rEnd, const vector< bool > &rEndInclusive, const Ordering &o, int direction ) {
        BSONObjIterator ll( l );
        BSONObjIterator rr( rBegin );
        vector< const BSONElement * >::const_iterator rr2 = rEnd.begin();
        vector< bool >::const_iterator inc = rEndInclusive.begin();
        unsigned mask = 1;
        for( int i = 0; i < rBeginLen; ++i, mask <<= 1 ) {
            BSONElement lll = ll.next();
            BSONElement rrr = rr.next();
            ++rr2;
            ++inc;
            
            int x = lll.woCompare( rrr, false );
            if ( o.descending( mask ) )
                x = -x;
            if ( x != 0 )
                return x;
        }
        if ( rSup ) {
            return -direction;
        }
        for( ; ll.more(); mask <<= 1 ) {
            BSONElement lll = ll.next();
            BSONElement rrr = **rr2;
            ++rr2;
            int x = lll.woCompare( rrr, false );
            if ( o.descending( mask ) )
                x = -x;
            if ( x != 0 )
                return x;
            if ( !*inc ) {
                return -direction;
            }
            ++inc;
        }
        return 0;
    }

    bool BtreeBucket::exists(const IndexDetails& idx, const DiskLoc &thisLoc, const BSONObj& key, const Ordering& order) const { 
        int pos;
        bool found;
        DiskLoc b = locate(idx, thisLoc, key, order, pos, found, minDiskLoc);

        // skip unused keys
        while ( 1 ) {
            if( b.isNull() )
                break;
            const BtreeBucket *bucket = b.btree();
            const _KeyNode& kn = bucket->k(pos);
            if ( kn.isUsed() )
                return bucket->keyAt(pos).woEqual(key);
            b = bucket->advance(b, pos, 1, "BtreeBucket::exists");
        }
        return false;
    }

    /* @param self - don't complain about ourself already being in the index case.
       @return true = there is a duplicate.
    */
    bool BtreeBucket::wouldCreateDup(
        const IndexDetails& idx, const DiskLoc &thisLoc, 
        const BSONObj& key, const Ordering& order,
        const DiskLoc &self) const
    { 
        int pos;
        bool found;
        DiskLoc b = locate(idx, thisLoc, key, order, pos, found, minDiskLoc);

        while ( !b.isNull() ) {
            // we skip unused keys
            const BtreeBucket *bucket = b.btree();
            const _KeyNode& kn = bucket->k(pos);
            if ( kn.isUsed() ) {
                if( bucket->keyAt(pos).woEqual(key) )
                    return kn.recordLoc != self;
                break;
            }
            b = bucket->advance(b, pos, 1, "BtreeBucket::dupCheck");
        }

        return false;
    }

    string BtreeBucket::dupKeyError( const IndexDetails& idx , const BSONObj& key ){
        stringstream ss;
        ss << "E11000 duplicate key error ";
        ss << "index: " << idx.indexNamespace() << "  ";
        ss << "dup key: " << key;
        return ss.str();
    }

    /* Find a key withing this btree bucket.
 
       When duplicate keys are allowed, we use the DiskLoc of the record as if it were part of the 
       key.  That assures that even when there are many duplicates (e.g., 1 million) for a key,
       our performance is still good.

       assertIfDup: if the key exists (ignoring the recordLoc), uassert

       pos: for existing keys k0...kn-1.
       returns # it goes BEFORE.  so key[pos-1] < key < key[pos]
       returns n if it goes after the last existing key.
       note result might be an Unused location!
    */
	char foo;
    bool BtreeBucket::find(const IndexDetails& idx, const BSONObj& key, const DiskLoc &recordLoc, const Ordering &order, int& pos, bool assertIfDup) const {
#if defined(_EXPERIMENT1)
		{
			char *z = (char *) this;
			int i = 0;
			while( 1 ) {
				i += 4096;
				if( i >= BucketSize )
					break;
				foo += z[i];
			}
		}
#endif
        
        globalIndexCounters.btree( (char*)this );
        
        /* binary search for this key */
        bool dupsChecked = false;
        int l=0;
        int h=n-1;
        while ( l <= h ) {
            int m = (l+h)/2;
            KeyNode M = keyNode(m);
            int x = key.woCompare(M.key, order);
            if ( x == 0 ) { 
                if( assertIfDup ) {
                    if( k(m).isUnused() ) { 
                        // ok that key is there if unused.  but we need to check that there aren't other 
                        // entries for the key then.  as it is very rare that we get here, we don't put any 
                        // coding effort in here to make this particularly fast
                        if( !dupsChecked ) { 
                            dupsChecked = true;
                            if( idx.head.btree()->exists(idx, idx.head, key, order) ) {
                                if( idx.head.btree()->wouldCreateDup(idx, idx.head, key, order, recordLoc) )
                                    uasserted( ASSERT_ID_DUPKEY , dupKeyError( idx , key ) );
                                else
                                    alreadyInIndex();
                            }
                        }
                    }
                    else {
                        if( M.recordLoc == recordLoc ) 
                            alreadyInIndex();
                        uasserted( ASSERT_ID_DUPKEY , dupKeyError( idx , key ) );
                    }
                }

                // dup keys allowed.  use recordLoc as if it is part of the key
                DiskLoc unusedRL = M.recordLoc;
                unusedRL.GETOFS() &= ~1; // so we can test equality without the used bit messing us up
                x = recordLoc.compare(unusedRL);
            }
            if ( x < 0 ) // key < M.key
                h = m-1;
            else if ( x > 0 )
                l = m+1;
            else {
                // found it.
                pos = m;
                return true;
            }
        }
        // not found
        pos = l;
        if ( pos != n ) {
            BSONObj keyatpos = keyNode(pos).key;
            wassert( key.woCompare(keyatpos, order) <= 0 );
            if ( pos > 0 ) {
                wassert( keyNode(pos-1).key.woCompare(key, order) <= 0 );
            }
        }

        return false;
    }

    void BtreeBucket::delBucket(const DiskLoc thisLoc, const IndexDetails& id) {
        ClientCursor::informAboutToDeleteBucket(thisLoc); // slow...
        assert( !isHead() );

        const BtreeBucket *p = parent.btree();
        int parentIdx = indexInParent( thisLoc );
        p->childForPos( parentIdx ).writing().Null();
        deallocBucket( thisLoc, id );
    }
    
    void BtreeBucket::deallocBucket(const DiskLoc thisLoc, const IndexDetails &id) {
#if 0
        /* as a temporary defensive measure, we zap the whole bucket, AND don't truly delete
           it (meaning it is ineligible for reuse).
           */
        memset(this, 0, Size());
#else
        //defensive:
        n = -1;
        parent.Null();
        string ns = id.indexNamespace();
        theDataFileMgr._deleteRecord(nsdetails(ns.c_str()), ns.c_str(), thisLoc.rec(), thisLoc);
#endif
    }

    /* note: may delete the entire bucket!  this invalid upon return sometimes. */
    void BtreeBucket::delKeyAtPos( const DiskLoc thisLoc, IndexDetails& id, int p, const Ordering &order) {
        assert(n>0);
        DiskLoc left = childForPos(p);

        if ( n == 1 ) {
            if ( left.isNull() && nextChild.isNull() ) {
                if ( isHead() ) {
                    _delKeyAtPos(p); // we don't delete the top bucket ever
                } else {
                    delBucket(thisLoc, id);
                }
                return;
            }
            markUnused(p);
            return;
        }

        if ( left.isNull() ) {
            _delKeyAtPos(p);
            balanceWithNeighbors( thisLoc, id, order );
        } else {
            markUnused(p);
        }
    }

    void BtreeBucket::replaceWithNextChild( const DiskLoc thisLoc, IndexDetails &id ) {
        assert( n == 0 && !nextChild.isNull() );
        if ( parent.isNull() ) {
            assert( id.head == thisLoc );
            id.head.writing() = nextChild;
        } else {
            parent.btree()->childForPos( indexInParent( thisLoc ) ).writing() = nextChild;
        }
        nextChild.btree()->parent.writing() = parent;
        ClientCursor::informAboutToDeleteBucket( thisLoc );
        deallocBucket( thisLoc, id );
    }
    
    bool BtreeBucket::tryMergeNeighbors( const DiskLoc thisLoc, int leftIndex, IndexDetails &id, const Ordering &order ) const {
        assert( leftIndex >= 0 && leftIndex < n );
        DiskLoc leftNodeLoc = childForPos( leftIndex );
        DiskLoc rightNodeLoc = childForPos( leftIndex + 1 );
        if ( leftNodeLoc.isNull() || rightNodeLoc.isNull() ) {
            // TODO if this situation is possible in long term implementation, maybe we should compact somehow anyway
            return false;
        }
        int pos = 0;
        {
            const BtreeBucket *l = leftNodeLoc.btree();
            const BtreeBucket *r = rightNodeLoc.btree();
            if ( ( headerSize() + l->packedDataSize( pos ) + r->packedDataSize( pos ) + keyNode( leftIndex ).key.objsize() + sizeof(_KeyNode) > unsigned( BucketSize ) ) ) {
                return false;
            }
        }
        thisLoc.btreemod()->doMergeNeighbors( thisLoc, leftIndex, id, order );
        return true;
    }

    void BtreeBucket::doMergeNeighbors( const DiskLoc thisLoc, int leftIndex, IndexDetails &id, const Ordering &order ) {
        DiskLoc leftNodeLoc = childForPos( leftIndex );
        DiskLoc rightNodeLoc = childForPos( leftIndex + 1 );
        BtreeBucket *l = leftNodeLoc.btreemod();
        BtreeBucket *r = rightNodeLoc.btreemod();
        int pos = 0;
        l->pack( order, pos );
        r->pack( order, pos ); // pack r in case there are droppable keys
        
        int oldLNum = l->n;
        {
            KeyNode kn = keyNode( leftIndex );
            l->pushBack( kn.recordLoc, kn.key, order, l->nextChild ); // left child's right child becomes old parent key's left child
        }
        for( int i = 0; i < r->n; ++i ) {
            KeyNode kn = r->keyNode( i );
            l->pushBack( kn.recordLoc, kn.key, order, kn.prevChildBucket );
        }
        l->nextChild = r->nextChild;
        l->fixParentPtrs( leftNodeLoc, oldLNum );
        r->delBucket( rightNodeLoc, id );
        childForPos( leftIndex + 1 ) = leftNodeLoc;
        childForPos( leftIndex ) = DiskLoc();
        _delKeyAtPos( leftIndex, true );
        if ( n == 0 ) {
            // will trash this and thisLoc
            replaceWithNextChild( thisLoc, id );
        } else {
            // balance recursively - maybe we should do this even when n == 0?
            balanceWithNeighbors( thisLoc, id, order );
        }
    }
    
    int BtreeBucket::indexInParent( const DiskLoc &thisLoc ) const {
        assert( !parent.isNull() );
        const BtreeBucket *p = parent.btree();
        if ( p->nextChild == thisLoc ) {
            return p->n;
        } else {
            for( int i = 0; i < p->n; ++i ) {
                if ( p->k( i ).prevChildBucket == thisLoc ) {
                    return i;
                }
            }
        }
        out() << "ERROR: can't find ref to child bucket.\n";
        out() << "child: " << thisLoc << "\n";
        dump();
        out() << "Parent: " << parent << "\n";
        p->dump();
        assert(false);
        return -1; // just to compile
    }
    
    void BtreeBucket::balanceWithNeighbors( const DiskLoc thisLoc, IndexDetails &id, const Ordering &order ) const {
        if ( parent.isNull() ) { // we are root, there are no neighbors
            return;
        }
        const BtreeBucket *p = parent.btree();
        int parentIdx = indexInParent( thisLoc );
        if ( parentIdx < p->n ) {
            if ( p->tryMergeNeighbors( parent, parentIdx, id, order ) ) {
                return;
            }
        }
        if ( parentIdx > 0 ) {
            p->tryMergeNeighbors( parent, parentIdx - 1, id, order );
        }
    }
    
    /* remove a key from the index */
    bool BtreeBucket::unindex(const DiskLoc thisLoc, IndexDetails& id, const BSONObj& key, const DiskLoc recordLoc ) const {
        if ( key.objsize() > KeyMax ) {
            OCCASIONALLY problem() << "unindex: key too large to index, skipping " << id.indexNamespace() << /* ' ' << key.toString() << */ endl;
            return false;
        }

        int pos;
        bool found;
        DiskLoc loc = locate(id, thisLoc, key, Ordering::make(id.keyPattern()), pos, found, recordLoc, 1);
        if ( found ) {
            loc.btreemod()->delKeyAtPos(loc, id, pos, Ordering::make(id.keyPattern()));
            return true;
        }
        return false;
    }

    BtreeBucket* BtreeBucket::allocTemp() {
        BtreeBucket *b = (BtreeBucket*) malloc(BucketSize);
        b->init();
        return b;
    }

    inline void BtreeBucket::fix(const DiskLoc thisLoc, const DiskLoc child) {
        if ( !child.isNull() ) {
            if ( insert_debug )
                out() << "      " << child.toString() << ".parent=" << thisLoc.toString() << endl;
            child.btree()->parent.writing() = thisLoc;
        }
    }

    /* this sucks.  maybe get rid of parent ptrs. */
    void BtreeBucket::fixParentPtrs(const DiskLoc thisLoc, int startIndex) const {
        VERIFYTHISLOC
        fix(thisLoc, nextChild);
        for ( int i = startIndex; i < n; i++ )
            fix(thisLoc, k(i).prevChildBucket);
    }

    void BtreeBucket::_insertHere(DiskLoc thisLoc, int keypos,
                                 DiskLoc recordLoc, const BSONObj& key, const Ordering& order,
                                 DiskLoc lchild, DiskLoc rchild, IndexDetails& idx)
    {
        if ( insert_debug )
            out() << "   " << thisLoc.toString() << ".insertHere " << key.toString() << '/' << recordLoc.toString() << ' '
                 << lchild.toString() << ' ' << rchild.toString() << " keypos:" << keypos << endl;

        DiskLoc oldLoc = thisLoc;

        if ( basicInsert(thisLoc, keypos, recordLoc, key, order) ) {
            _KeyNode& kn = k(keypos);
            if ( keypos+1 == n ) { // last key
                if ( nextChild != lchild ) {
                    out() << "ERROR nextChild != lchild" << endl;
                    out() << "  thisLoc: " << thisLoc.toString() << ' ' << idx.indexNamespace() << endl;
                    out() << "  keyPos: " << keypos << " n:" << n << endl;
                    out() << "  nextChild: " << nextChild.toString() << " lchild: " << lchild.toString() << endl;
                    out() << "  recordLoc: " << recordLoc.toString() << " rchild: " << rchild.toString() << endl;
                    out() << "  key: " << key.toString() << endl;
                    dump();
#if 0
                    out() << "\n\nDUMPING FULL INDEX" << endl;
                    bt_dmp=1;
                    bt_fv=1;
                    idx.head.btree()->fullValidate(idx.head);
#endif
                    assert(false);
                }
                kn.prevChildBucket = nextChild;
                assert( kn.prevChildBucket == lchild );
                nextChild = rchild;
                if ( !rchild.isNull() )
                    rchild.btree()->parent.writing() = thisLoc;
            }
            else {
                k(keypos).prevChildBucket = lchild;
                if ( k(keypos+1).prevChildBucket != lchild ) {
                    out() << "ERROR k(keypos+1).prevChildBucket != lchild" << endl;
                    out() << "  thisLoc: " << thisLoc.toString() << ' ' << idx.indexNamespace() << endl;
                    out() << "  keyPos: " << keypos << " n:" << n << endl;
                    out() << "  k(keypos+1).pcb: " << k(keypos+1).prevChildBucket.toString() << " lchild: " << lchild.toString() << endl;
                    out() << "  recordLoc: " << recordLoc.toString() << " rchild: " << rchild.toString() << endl;
                    out() << "  key: " << key.toString() << endl;
                    dump();
#if 0
                    out() << "\n\nDUMPING FULL INDEX" << endl;
                    bt_dmp=1;
                    bt_fv=1;
                    idx.head.btree()->fullValidate(idx.head);
#endif
                    assert(false);
                }
                k(keypos+1).prevChildBucket = rchild;
                if ( !rchild.isNull() )
                    rchild.btree()->parent.writing() = thisLoc;
            }
            return;
        }

        /* ---------- split ---------------- */

        if ( split_debug )
            out() << "    " << thisLoc.toString() << ".split" << endl;

        int split = splitPos( keypos );
        DiskLoc rLoc = addBucket(idx);
        BtreeBucket *r = rLoc.btreemod();
        if ( split_debug )
            out() << "     split:" << split << ' ' << keyNode(split).key.toString() << " n:" << n << endl;
        for ( int i = split+1; i < n; i++ ) {
            KeyNode kn = keyNode(i);
            r->pushBack(kn.recordLoc, kn.key, order, kn.prevChildBucket);
        }
        r->nextChild = nextChild;
        r->assertValid( order );

        if ( split_debug )
            out() << "     new rLoc:" << rLoc.toString() << endl;
        r = 0;
        rLoc.btree()->fixParentPtrs(rLoc);

        {
            KeyNode splitkey = keyNode(split);
            nextChild = splitkey.prevChildBucket; // splitkey key gets promoted, its children will be thisLoc (l) and rLoc (r)
            if ( split_debug ) {
                out() << "    splitkey key:" << splitkey.key.toString() << endl;
            }
            
            // promote splitkey to a parent node
            if ( parent.isNull() ) {
                // make a new parent if we were the root
                DiskLoc L = addBucket(idx);
                BtreeBucket *p = L.btreemod();
                p->pushBack(splitkey.recordLoc, splitkey.key, order, thisLoc);
                p->nextChild = rLoc;
                p->assertValid( order );
                parent = idx.head.writing() = L;
                if ( split_debug )
                    out() << "    we were root, making new root:" << hex << parent.getOfs() << dec << endl;
                rLoc.btree()->parent.writing() = parent;
            }
            else {
                /* set this before calling _insert - if it splits it will do fixParent() logic and change the value.
                */
                rLoc.btree()->parent.writing() = parent;
                if ( split_debug )
                    out() << "    promoting splitkey key " << splitkey.key.toString() << endl;
                parent.btree()->_insert(parent, splitkey.recordLoc, splitkey.key, order, /*dupsallowed*/true, thisLoc, rLoc, idx);
            }
        }

        int newpos = keypos;
        // note this may trash splitkey.key.  thus we had to promote it before finishing up here.
        truncateTo(split, order, newpos);  // note this may trash splitkey.key.  thus we had to promote it before finishing up here.

        // add our new key, there is room now
        {
            if ( keypos <= split ) {
                if ( split_debug )
                    out() << "  keypos<split, insertHere() the new key" << endl;
                insertHere(thisLoc, newpos, recordLoc, key, order, lchild, rchild, idx);
            } else {
                int kp = keypos-split-1;
                assert(kp>=0);
                rLoc.btree()->insertHere(rLoc, kp, recordLoc, key, order, lchild, rchild, idx);
            }
        }

        if ( split_debug )
            out() << "     split end " << hex << thisLoc.getOfs() << dec << endl;
    }

    /* insert a key in this bucket, splitting if necessary.
       keypos - where to insert the key i3n range 0..n.  0=make leftmost, n=make rightmost.
       NOTE this function may free some data, and as a result the value passed for keypos may
       be invalid after calling insertHere()
    */
    void BtreeBucket::insertHere(DiskLoc thisLoc, int keypos,
                                 DiskLoc recordLoc, const BSONObj& key, const Ordering& order,
                                 DiskLoc lchild, DiskLoc rchild, IndexDetails& idx) const
    {
        thisLoc.btreemod()->_insertHere(thisLoc, keypos, recordLoc, key, order, lchild, rchild, idx);
    }

    /* start a new index off, empty */
    DiskLoc BtreeBucket::addBucket(const IndexDetails& id) {
        string ns = id.indexNamespace();
        DiskLoc loc = theDataFileMgr.insert(ns.c_str(), 0, BucketSize, true);
        BtreeBucket *b = loc.btreemod();
        b->init();
        return loc;
    }

    void BtreeBucket::renameIndexNamespace(const char *oldNs, const char *newNs) {
        renameNamespace( oldNs, newNs );
    }

    DiskLoc BtreeBucket::getHead(const DiskLoc& thisLoc) const {
        DiskLoc p = thisLoc;
        while ( !p.btree()->isHead() )
            p = p.btree()->parent;
        return p;
    }

    DiskLoc BtreeBucket::advance(const DiskLoc& thisLoc, int& keyOfs, int direction, const char *caller) const {
        if ( keyOfs < 0 || keyOfs >= n ) {
            out() << "ASSERT failure BtreeBucket::advance, caller: " << caller << endl;
            out() << "  thisLoc: " << thisLoc.toString() << endl;
            out() << "  keyOfs: " << keyOfs << " n:" << n << " direction: " << direction << endl;
            out() << bucketSummary() << endl;
            assert(false);
        }
        int adj = direction < 0 ? 1 : 0;
        int ko = keyOfs + direction;
        DiskLoc nextDown = childForPos(ko+adj);
        if ( !nextDown.isNull() ) {
            while ( 1 ) {
                keyOfs = direction>0 ? 0 : nextDown.btree()->n - 1;
                DiskLoc loc = nextDown.btree()->childForPos(keyOfs + adj);
                if ( loc.isNull() )
                    break;
                nextDown = loc;
            }
            return nextDown;
        }

        if ( ko < n && ko >= 0 ) {
            keyOfs = ko;
            return thisLoc;
        }

        // end of bucket.  traverse back up.
        DiskLoc childLoc = thisLoc;
        DiskLoc ancestor = parent;
        while ( 1 ) {
            if ( ancestor.isNull() )
                break;
            const BtreeBucket *an = ancestor.btree();
            for ( int i = 0; i < an->n; i++ ) {
                if ( an->childForPos(i+adj) == childLoc ) {
                    keyOfs = i;
                    return ancestor;
                }
            }
            assert( direction<0 || an->nextChild == childLoc );
            // parent exhausted also, keep going up
            childLoc = ancestor;
            ancestor = an->parent;
        }

        return DiskLoc();
    }

    DiskLoc BtreeBucket::locate(const IndexDetails& idx, const DiskLoc& thisLoc, const BSONObj& key, const Ordering &order, int& pos, bool& found, const DiskLoc &recordLoc, int direction) const {
        int p;
        found = find(idx, key, recordLoc, order, p, /*assertIfDup*/ false);
        if ( found ) {
            pos = p;
            return thisLoc;
        }

        DiskLoc child = childForPos(p);

        if ( !child.isNull() ) {
            DiskLoc l = child.btree()->locate(idx, child, key, order, pos, found, recordLoc, direction);
            if ( !l.isNull() )
                return l;
        }

        pos = p;
        if ( direction < 0 )
            return --pos == -1 ? DiskLoc() /*theend*/ : thisLoc;
        else
            return pos == n ? DiskLoc() /*theend*/ : thisLoc;
    }
    
    bool BtreeBucket::customFind( int l, int h, const BSONObj &keyBegin, int keyBeginLen, bool afterKey, const vector< const BSONElement * > &keyEnd, const vector< bool > &keyEndInclusive, const Ordering &order, int direction, DiskLoc &thisLoc, int &keyOfs, pair< DiskLoc, int > &bestParent ) const {
        while( 1 ) {
            if ( l + 1 == h ) {
                keyOfs = ( direction > 0 ) ? h : l;
                DiskLoc next = thisLoc.btree()->k( h ).prevChildBucket;
                if ( !next.isNull() ) {
                    bestParent = make_pair( thisLoc, keyOfs );
                    thisLoc = next;
                    return true;
                } else {
                    return false;
                }
            }
            int m = l + ( h - l ) / 2;
            int cmp = customBSONCmp( thisLoc.btree()->keyNode( m ).key, keyBegin, keyBeginLen, afterKey, keyEnd, keyEndInclusive, order, direction );
            if ( cmp < 0 ) {
                l = m;
            } else if ( cmp > 0 ) {
                h = m;
            } else {
                if ( direction < 0 ) {
                    l = m;
                } else {
                    h = m;
                }
            }
        }        
    }
    
    // find smallest/biggest value greater-equal/less-equal than specified
    // starting thisLoc + keyOfs will be strictly less than/strictly greater than keyBegin/keyBeginLen/keyEnd
    // All the direction checks below allowed me to refactor the code, but possibly separate forward and reverse implementations would be more efficient
    void BtreeBucket::advanceTo(DiskLoc &thisLoc, int &keyOfs, const BSONObj &keyBegin, int keyBeginLen, bool afterKey, const vector< const BSONElement * > &keyEnd, const vector< bool > &keyEndInclusive, const Ordering &order, int direction ) const {
        int l,h;
        bool dontGoUp;
        if ( direction > 0 ) {
            l = keyOfs;
            h = n - 1;
            dontGoUp = ( customBSONCmp( keyNode( h ).key, keyBegin, keyBeginLen, afterKey, keyEnd, keyEndInclusive, order, direction ) >= 0 );
        } else {
            l = 0;
            h = keyOfs;
            dontGoUp = ( customBSONCmp( keyNode( l ).key, keyBegin, keyBeginLen, afterKey, keyEnd, keyEndInclusive, order, direction ) <= 0 );
        }
        pair< DiskLoc, int > bestParent;
        if ( dontGoUp ) {
            // this comparison result assures h > l
            if ( !customFind( l, h, keyBegin, keyBeginLen, afterKey, keyEnd, keyEndInclusive, order, direction, thisLoc, keyOfs, bestParent ) ) {
                return;
            }
        } else {
            // go up parents until rightmost/leftmost node is >=/<= target or at top
            while( !thisLoc.btree()->parent.isNull() ) {
                thisLoc = thisLoc.btree()->parent;
                if ( direction > 0 ) {
                    if ( customBSONCmp( thisLoc.btree()->keyNode( thisLoc.btree()->n - 1 ).key, keyBegin, keyBeginLen, afterKey, keyEnd, keyEndInclusive, order, direction ) >= 0 ) {
                        break;
                    }
                } else {
                    if ( customBSONCmp( thisLoc.btree()->keyNode( 0 ).key, keyBegin, keyBeginLen, afterKey, keyEnd, keyEndInclusive, order, direction ) <= 0 ) {
                        break;
                    }                    
                }
            }
        }
        customLocate( thisLoc, keyOfs, keyBegin, keyBeginLen, afterKey, keyEnd, keyEndInclusive, order, direction, bestParent );
    }
    
    void BtreeBucket::customLocate(DiskLoc &thisLoc, int &keyOfs, const BSONObj &keyBegin, int keyBeginLen, bool afterKey, const vector< const BSONElement * > &keyEnd, const vector< bool > &keyEndInclusive, const Ordering &order, int direction, pair< DiskLoc, int > &bestParent ) const {
        if ( thisLoc.btree()->n == 0 ) {
            thisLoc = DiskLoc();
            return;
        }
        // go down until find smallest/biggest >=/<= target
        while( 1 ) {
            int l = 0;
            int h = thisLoc.btree()->n - 1;
            // leftmost/rightmost key may possibly be >=/<= search key
            bool firstCheck;
            if ( direction > 0 ) {
                firstCheck = ( customBSONCmp( thisLoc.btree()->keyNode( 0 ).key, keyBegin, keyBeginLen, afterKey, keyEnd, keyEndInclusive, order, direction ) >= 0 );
            } else {
                firstCheck = ( customBSONCmp( thisLoc.btree()->keyNode( h ).key, keyBegin, keyBeginLen, afterKey, keyEnd, keyEndInclusive, order, direction ) <= 0 );
            }
            if ( firstCheck ) {
                DiskLoc next;
                if ( direction > 0 ) {
                    next = thisLoc.btree()->k( 0 ).prevChildBucket;
                    keyOfs = 0;
                } else {
                    next = thisLoc.btree()->nextChild;
                    keyOfs = h;
                }
                if ( !next.isNull() ) {
                    bestParent = make_pair( thisLoc, keyOfs );
                    thisLoc = next;
                    continue;
                } else {
                    return;
                }
            }
            bool secondCheck;
            if ( direction > 0 ) {
                secondCheck = ( customBSONCmp( thisLoc.btree()->keyNode( h ).key, keyBegin, keyBeginLen, afterKey, keyEnd, keyEndInclusive, order, direction ) < 0 );
            } else {
                secondCheck = ( customBSONCmp( thisLoc.btree()->keyNode( 0 ).key, keyBegin, keyBeginLen, afterKey, keyEnd, keyEndInclusive, order, direction ) > 0 );
            }
            if ( secondCheck ) {
                DiskLoc next;
                if ( direction > 0 ) {
                    next = thisLoc.btree()->nextChild;
                } else {
                    next = thisLoc.btree()->k( 0 ).prevChildBucket;
                }
                if ( next.isNull() ) {
                    // if bestParent is null, we've hit the end and thisLoc gets set to DiskLoc()
                    thisLoc = bestParent.first;
                    keyOfs = bestParent.second;
                    return;
                } else {
                    thisLoc = next;
                    continue;
                }
            }
            if ( !customFind( l, h, keyBegin, keyBeginLen, afterKey, keyEnd, keyEndInclusive, order, direction, thisLoc, keyOfs, bestParent ) ) {
                return;
            }
        }
    }

    
    /* @thisLoc disk location of *this
    */
    int BtreeBucket::_insert(DiskLoc thisLoc, DiskLoc recordLoc,
                             const BSONObj& key, const Ordering &order, bool dupsAllowed,
                             DiskLoc lChild, DiskLoc rChild, IndexDetails& idx) const {
        if ( key.objsize() > KeyMax ) {
            problem() << "ERROR: key too large len:" << key.objsize() << " max:" << KeyMax << ' ' << key.objsize() << ' ' << idx.indexNamespace() << endl;
            return 2;
        }
        assert( key.objsize() > 0 );

        int pos;
        bool found = find(idx, key, recordLoc, order, pos, !dupsAllowed);
        if ( insert_debug ) {
            out() << "  " << thisLoc.toString() << '.' << "_insert " <<
                 key.toString() << '/' << recordLoc.toString() <<
                 " l:" << lChild.toString() << " r:" << rChild.toString() << endl;
            out() << "    found:" << found << " pos:" << pos << " n:" << n << endl;
        }

        if ( found ) {
            const _KeyNode& kn = k(pos);
            if ( kn.isUnused() ) {
                log(4) << "btree _insert: reusing unused key" << endl;
                massert( 10285 , "_insert: reuse key but lchild is not null", lChild.isNull());
                massert( 10286 , "_insert: reuse key but rchild is not null", rChild.isNull());
                kn.writing().setUsed();
                return 0;
            }

            DEV { 
                log() << "_insert(): key already exists in index (ok for background:true)\n";
                log() << "  " << idx.indexNamespace() << " thisLoc:" << thisLoc.toString() << '\n';
                log() << "  " << key.toString() << '\n';
                log() << "  " << "recordLoc:" << recordLoc.toString() << " pos:" << pos << endl;
                log() << "  old l r: " << childForPos(pos).toString() << ' ' << childForPos(pos+1).toString() << endl;
                log() << "  new l r: " << lChild.toString() << ' ' << rChild.toString() << endl;
            }
            alreadyInIndex();
        }

        DEBUGGING out() << "TEMP: key: " << key.toString() << endl;
        DiskLoc child = childForPos(pos);
        if ( insert_debug )
            out() << "    getChild(" << pos << "): " << child.toString() << endl;
        if ( child.isNull() || !rChild.isNull() /* means an 'internal' insert */ ) {
            insertHere(thisLoc, pos, recordLoc, key, order, lChild, rChild, idx);
            return 0;
        }

        return child.btree()->bt_insert(child, recordLoc, key, order, dupsAllowed, idx, /*toplevel*/false);
    }

    void BtreeBucket::dump() const {
        out() << "DUMP btreebucket n:" << n;
        out() << " parent:" << hex << parent.getOfs() << dec;
        for ( int i = 0; i < n; i++ ) {
            out() << '\n';
            KeyNode k = keyNode(i);
            out() << '\t' << i << '\t' << k.key.toString() << "\tleft:" << hex <<
                 k.prevChildBucket.getOfs() << "\tRecLoc:" << k.recordLoc.toString() << dec;
            if ( this->k(i).isUnused() )
                out() << " UNUSED";
        }
        out() << " right:" << hex << nextChild.getOfs() << dec << endl;
    }

    /* todo: meaning of return code unclear clean up */
    int BtreeBucket::bt_insert(const DiskLoc thisLoc, const DiskLoc recordLoc,
                            const BSONObj& key, const Ordering &order, bool dupsAllowed,
                            IndexDetails& idx, bool toplevel) const
    {
        if ( toplevel ) {
            if ( key.objsize() > KeyMax ) {
                problem() << "Btree::insert: key too large to index, skipping " << idx.indexNamespace() << ' ' << key.objsize() << ' ' << key.toString() << endl;
                return 3;
            }
        }

        int x = _insert(thisLoc, recordLoc, key, order, dupsAllowed, DiskLoc(), DiskLoc(), idx);
        assertValid( order );

        return x;
    }

    void BtreeBucket::shape(stringstream& ss) const {
        _shape(0, ss);
    }
    
    DiskLoc BtreeBucket::findSingle( const IndexDetails& indexdetails , const DiskLoc& thisLoc, const BSONObj& key ) const {
        int pos;
        bool found;
        /* TODO: is it really ok here that the order is a default? */
        Ordering o = Ordering::make(BSONObj());
        DiskLoc bucket = locate( indexdetails , indexdetails.head , key , o , pos , found , minDiskLoc );
        if ( bucket.isNull() )
            return bucket;

        const BtreeBucket *b = bucket.btree();
        while ( 1 ){
            const _KeyNode& knraw = b->k(pos);
            if ( knraw.isUsed() )
                break;
            bucket = b->advance( bucket , pos , 1 , "findSingle" );
            if ( bucket.isNull() )
                return bucket;
            b = bucket.btree();
        }
        KeyNode kn = b->keyNode( pos );
        if ( key.woCompare( kn.key ) != 0 )
            return DiskLoc();
        return kn.recordLoc;
    }

} // namespace mongo

#include "db.h"
#include "dbhelpers.h"

namespace mongo {

    void BtreeBucket::a_test(IndexDetails& id) {
        BtreeBucket *b = id.head.btreemod();

        // record locs for testing
        DiskLoc A(1, 20);
        DiskLoc B(1, 30);
        DiskLoc C(1, 40);

        DiskLoc rl;
        BSONObj key = fromjson("{x:9}");
        BSONObj orderObj = fromjson("{}");
        Ordering order = Ordering::make(orderObj);

        b->bt_insert(id.head, A, key, order, true, id);
        A.GETOFS() += 2;
        b->bt_insert(id.head, A, key, order, true, id);
        A.GETOFS() += 2;
        b->bt_insert(id.head, A, key, order, true, id);
        A.GETOFS() += 2;
        b->bt_insert(id.head, A, key, order, true, id);
        A.GETOFS() += 2;
        assert( b->k(0).isUsed() );
//        b->k(0).setUnused();
        b->k(1).setUnused();
        b->k(2).setUnused();
        b->k(3).setUnused();

        b->dumpTree(id.head, orderObj);

        /*        b->bt_insert(id.head, B, key, order, false, id);
        b->k(1).setUnused();

        b->dumpTree(id.head, order);

        b->bt_insert(id.head, A, key, order, false, id);

        b->dumpTree(id.head, order);
        */

        // this should assert.  does it? (it might "accidentally" though, not asserting proves a problem, asserting proves nothing)
        b->bt_insert(id.head, C, key, order, false, id);

//        b->dumpTree(id.head, order);
    }

    /* --- BtreeBuilder --- */

    BtreeBuilder::BtreeBuilder(bool _dupsAllowed, IndexDetails& _idx) : 
      dupsAllowed(_dupsAllowed), 
      idx(_idx), 
      n(0),
      order( idx.keyPattern() ),
      ordering( Ordering::make(idx.keyPattern()) )
    {
        first = cur = BtreeBucket::addBucket(idx);
        b = cur.btreemod();
        committed = false;
    }

    void BtreeBuilder::newBucket() { 
        DiskLoc L = BtreeBucket::addBucket(idx);
        b->tempNext() = L;
        cur = L;
        b = cur.btreemod();
    }

    void BtreeBuilder::addKey(BSONObj& key, DiskLoc loc) { 
        if( !dupsAllowed ) {
            if( n > 0 ) {
                int cmp = keyLast.woCompare(key, order);
                massert( 10288 ,  "bad key order in BtreeBuilder - server internal error", cmp <= 0 );
                if( cmp == 0 ) {
                    //if( !dupsAllowed )
                    uasserted( ASSERT_ID_DUPKEY , BtreeBucket::dupKeyError( idx , keyLast ) );
                }
            }
            keyLast = key;
        }

        if ( ! b->_pushBack(loc, key, ordering, DiskLoc()) ){
            // no room
            if ( key.objsize() > KeyMax ) {
                problem() << "Btree::insert: key too large to index, skipping " << idx.indexNamespace() << ' ' << key.objsize() << ' ' << key.toString() << endl;
            }
            else { 
                // bucket was full
                newBucket();
                b->pushBack(loc, key, ordering, DiskLoc());
            }
        }
        n++;
    }

    void BtreeBuilder::buildNextLevel(DiskLoc loc) { 
        int levels = 1;
        while( 1 ) { 
            if( loc.btree()->tempNext().isNull() ) { 
                // only 1 bucket at this level. we are done.
                dur::writingDiskLoc(idx.head) = loc;
                break;
            }
            levels++;

            DiskLoc upLoc = BtreeBucket::addBucket(idx);
            DiskLoc upStart = upLoc;
            BtreeBucket *up = upLoc.btreemod();

            DiskLoc xloc = loc;
            while( !xloc.isNull() ) { 
                BtreeBucket *x = xloc.btreemod();
                BSONObj k; 
                DiskLoc r;
                x->popBack(r,k);
                bool keepX = ( x->n != 0 );
                DiskLoc keepLoc = keepX ? xloc : x->nextChild;

                if ( ! up->_pushBack(r, k, ordering, keepLoc) ){
                    // current bucket full
                    DiskLoc n = BtreeBucket::addBucket(idx);
                    up->tempNext() = n;
                    upLoc = n; 
                    up = upLoc.btreemod();
                    up->pushBack(r, k, ordering, keepLoc);
                }

                DiskLoc nextLoc = x->tempNext(); /* get next in chain at current level */
                if ( keepX ) {
                    x->parent = upLoc;                
                } else {
                    if ( !x->nextChild.isNull() )
                        x->nextChild.btreemod()->parent = upLoc;
                    x->deallocBucket( xloc, idx );
                }
                xloc = nextLoc;
            }
            
            loc = upStart;
        }

        if( levels > 1 )
            log(2) << "btree levels: " << levels << endl;
    }

    /* when all addKeys are done, we then build the higher levels of the tree */
    void BtreeBuilder::commit() { 
        buildNextLevel(first);
        committed = true;
    }

    BtreeBuilder::~BtreeBuilder() { 
        if( !committed ) { 
            log(2) << "Rolling back partially built index space" << endl;
            DiskLoc x = first;
            while( !x.isNull() ) { 
                DiskLoc next = x.btree()->tempNext();
                string ns = idx.indexNamespace();
                theDataFileMgr._deleteRecord(nsdetails(ns.c_str()), ns.c_str(), x.rec(), x);
                x = next;
            }
            assert( idx.head.isNull() );
            log(2) << "done rollback" << endl;
        }
    }

}
