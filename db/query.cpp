// query.cpp

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

#include "stdafx.h"
#include "query.h"
#include "pdfile.h"
#include "jsobjmanipulator.h"
#include "../util/builder.h"
#include <time.h>
#include "introspect.h"
#include "btree.h"
#include "../util/lruishmap.h"
#include "json.h"
#include "repl.h"
#include "replset.h"
#include "scanandorder.h"
#include "security.h"
#include "curop.h"
#include "commands.h"
#include "queryoptimizer.h"
#include "lasterror.h"

namespace mongo {

    /* We cut off further objects once we cross this threshold; thus, you might get
       a little bit more than this, it is a threshold rather than a limit.
    */
    const int MaxBytesToReturnToClientAtOnce = 4 * 1024 * 1024;

    //ns->query->DiskLoc
//    LRUishMap<BSONObj,DiskLoc,5> lrutest(123);

    extern bool useCursors;
    extern bool useHints;

    // Just try to identify best plan.
    class DeleteOp : public QueryOp {
    public:
        DeleteOp( bool justOne, int& bestCount ) :
            justOne_( justOne ),
            count_(),
            bestCount_( bestCount ),
            nScanned_() {
        }
        virtual void init() {
            c_ = qp().newCursor();
            matcher_.reset( new CoveredIndexMatcher( qp().query(), qp().indexKey() ) );
        }
        virtual void next() {
            if ( !c_->ok() ) {
                setComplete();
                return;
            }
            
            DiskLoc rloc = c_->currLoc();
            
            if ( matcher_->matches(c_->currKey(), rloc ) ) {
                if ( !c_->getsetdup(rloc) )
                    ++count_;
            }

            c_->advance();
            ++nScanned_;
            if ( count_ > bestCount_ )
                bestCount_ = count_;
            
            if ( count_ > 0 ) {
                if ( justOne_ )
                    setComplete();
                else if ( nScanned_ >= 100 && count_ == bestCount_ )
                    setComplete();
            }
        }
        virtual bool mayRecordPlan() const { return !justOne_; }
        virtual QueryOp *clone() const {
            return new DeleteOp( justOne_, bestCount_ );
        }
        auto_ptr< Cursor > newCursor() const { return qp().newCursor(); }
    private:
        bool justOne_;
        int count_;
        int &bestCount_;
        long long nScanned_;
        auto_ptr< Cursor > c_;
        auto_ptr< CoveredIndexMatcher > matcher_;
    };
    
    /* ns:      namespace, e.g. <database>.<collection>
       pattern: the "where" clause / criteria
       justOne: stop after 1 match
    */
    int deleteObjects(const char *ns, BSONObj pattern, bool justOne, bool logop, bool god) {
        if( !god ) {
            if ( strstr(ns, ".system.") ) {
                /* note a delete from system.indexes would corrupt the db 
                if done here, as there are pointers into those objects in 
                NamespaceDetails.
                */
                uassert(12050, "cannot delete from system namespace", legalClientSystemNS( ns , true ) );
            }
            if ( strchr( ns , '$' ) ){
                log() << "cannot delete from collection with reserved $ in name: " << ns << endl;
                uassert( 10100 ,  "cannot delete from collection with reserved $ in name", strchr(ns, '$') == 0 );
            }
        }

        NamespaceDetails *d = nsdetails( ns );
        if ( ! d )
            return 0;
        uassert( 10101 ,  "can't remove from a capped collection" , ! d->capped );

        int nDeleted = 0;
        QueryPlanSet s( ns, pattern, BSONObj() );
        int best = 0;
        DeleteOp original( justOne, best );
        shared_ptr< DeleteOp > bestOp = s.runOp( original );
        auto_ptr< Cursor > creal = bestOp->newCursor();
        
        if( !creal->ok() )
            return nDeleted;

        CoveredIndexMatcher matcher(pattern, creal->indexKeyPattern());

        auto_ptr<ClientCursor> cc( new ClientCursor(creal, ns, false) );
        cc->setDoingDeletes( true );

        CursorId id = cc->cursorid;
        
        unsigned long long nScanned = 0;
        do {
            if ( ++nScanned % 128 == 0 && !matcher.docMatcher().atomic() ) {
                if ( ! cc->yield() ){
                    cc.release(); // has already been deleted elsewhere
                    break;
                }
            }
            
            // this way we can avoid calling updateLocation() every time (expensive)
            // as well as some other nuances handled
            cc->setDoingDeletes( true );
            
            DiskLoc rloc = cc->c->currLoc();
            BSONObj key = cc->c->currKey();
            
            cc->c->advance();
            
            if ( ! matcher.matches( key , rloc ) )
                continue;
            
            assert( !cc->c->getsetdup(rloc) ); // can't be a dup, we deleted it!

            if ( !justOne ) {
                /* NOTE: this is SLOW.  this is not good, noteLocation() was designed to be called across getMore
                   blocks.  here we might call millions of times which would be bad.
                */
                cc->c->noteLocation();
            }
            
            if ( logop ) {
                BSONElement e;
                if( BSONObj( rloc.rec() ).getObjectID( e ) ) {
                    BSONObjBuilder b;
                    b.append( e );
                    bool replJustOne = true;
                    logOp( "d", ns, b.done(), 0, &replJustOne );
                } else {
                    problem() << "deleted object without id, not logging" << endl;
                }
            }

            theDataFileMgr.deleteRecord(ns, rloc.rec(), rloc);
            nDeleted++;
            if ( justOne )
                break;
            cc->c->checkLocation();
            
        } while ( cc->c->ok() );

        if ( cc.get() && ClientCursor::find( id , false ) == 0 ){
            cc.release();
        }

        return nDeleted;
    }

    int otherTraceLevel = 0;

    int initialExtentSize(int len);

    bool runCommands(const char *ns, BSONObj& jsobj, CurOp& curop, BufBuilder &b, BSONObjBuilder& anObjBuilder, bool fromRepl, int queryOptions) {
        try {
            return _runCommands(ns, jsobj, b, anObjBuilder, fromRepl, queryOptions);
        }
        catch ( AssertionException& e ) {
            if ( !e.msg.empty() )
                anObjBuilder.append("assertion", e.msg);
        }
        curop.debug().str << " assertion ";
        anObjBuilder.append("errmsg", "db assertion failure");
        anObjBuilder.append("ok", 0.0);
        BSONObj x = anObjBuilder.done();
        b.append((void*) x.objdata(), x.objsize());
        return true;
    }

    int nCaught = 0;

    void killCursors(int n, long long *ids) {
        int k = 0;
        for ( int i = 0; i < n; i++ ) {
            if ( ClientCursor::erase(ids[i]) )
                k++;
        }
        log( k == n ) << "killcursors: found " << k << " of " << n << '\n';
    }

    BSONObj id_obj = fromjson("{\"_id\":ObjectId( \"000000000000000000000000\" )}");
    BSONObj empty_obj = fromjson("{}");

    /* This is for languages whose "objects" are not well ordered (JSON is well ordered).
       [ { a : ... } , { b : ... } ] -> { a : ..., b : ... }
    */
    inline BSONObj transformOrderFromArrayFormat(BSONObj order) {
        /* note: this is slow, but that is ok as order will have very few pieces */
        BSONObjBuilder b;
        char p[2] = "0";

        while ( 1 ) {
            BSONObj j = order.getObjectField(p);
            if ( j.isEmpty() )
                break;
            BSONElement e = j.firstElement();
            uassert( 10102 , "bad order array", !e.eoo());
            uassert( 10103 , "bad order array [2]", e.isNumber());
            b.append(e);
            (*p)++;
            uassert( 10104 , "too many ordering elements", *p <= '9');
        }

        return b.obj();
    }


    //int dump = 0;

    /* empty result for error conditions */
    QueryResult* emptyMoreResult(long long cursorid) {
        BufBuilder b(32768);
        b.skip(sizeof(QueryResult));
        QueryResult *qr = (QueryResult *) b.buf();
        qr->cursorId = 0; // 0 indicates no more data to retrieve.
        qr->startingFrom = 0;
        qr->len = b.len();
        qr->setOperation(opReply);
        qr->nReturned = 0;
        b.decouple();
        return qr;
    }

    QueryResult* getMore(const char *ns, int ntoreturn, long long cursorid , CurOp& curop ) {
        StringBuilder& ss = curop.debug().str;
        ClientCursor::Pointer p(cursorid);
        ClientCursor *cc = p._c;
        
        int bufSize = 512;
        if ( cc ){
            bufSize += sizeof( QueryResult );
            bufSize += ( ntoreturn ? 4 : 1 ) * 1024 * 1024;
        }
        BufBuilder b( bufSize );

        b.skip(sizeof(QueryResult));

        int resultFlags = 0; //QueryResult::ResultFlag_AwaitCapable;
        int start = 0;
        int n = 0;

        if ( !cc ) {
            log() << "getMore: cursorid not found " << ns << " " << cursorid << endl;
            cursorid = 0;
            resultFlags = QueryResult::ResultFlag_CursorNotFound;
        }
        else {
            ss << " query: " << cc->query << " ";
            start = cc->pos;
            Cursor *c = cc->c.get();
            c->checkLocation();
            while ( 1 ) {
                if ( !c->ok() ) {
                    if ( c->tailable() ) {
                        if ( c->advance() ) {
                            continue;
                        }
                        break;
                    }
                    p.release();
                    bool ok = ClientCursor::erase(cursorid);
                    assert(ok);
                    cursorid = 0;
                    cc = 0;
                    break;
                }
                if ( !cc->matcher->matches(c->currKey(), c->currLoc() ) ) {
                }
                else {
                    //out() << "matches " << c->currLoc().toString() << '\n';
                    if( c->getsetdup(c->currLoc()) ) {
                        //out() << "  but it's a dup \n";
                    }
                    else {
                        BSONObj js = c->current();
                        fillQueryResultFromObj(b, cc->filter.get(), js);
                        n++;
                        if ( (ntoreturn>0 && (n >= ntoreturn || b.len() > MaxBytesToReturnToClientAtOnce)) ||
                             (ntoreturn==0 && b.len()>1*1024*1024) ) {
                            c->advance();
                            cc->pos += n;
                            //cc->updateLocation();
                            break;
                        }
                    }
                }
                c->advance();
            }
            if ( cc ) {
                cc->updateLocation();
                cc->mayUpgradeStorage();
            }
        }

        QueryResult *qr = (QueryResult *) b.buf();
        qr->len = b.len();
        qr->setOperation(opReply);
        qr->_resultFlags() = resultFlags;
        qr->cursorId = cursorid;
        qr->startingFrom = start;
        qr->nReturned = n;
        b.decouple();

        return qr;
    }

    class CountOp : public QueryOp {
    public:
        CountOp( const BSONObj &spec ) : spec_( spec ), count_(), bc_() {}
        virtual void init() {
            query_ = spec_.getObjectField( "query" );
            c_ = qp().newCursor();
            matcher_.reset( new CoveredIndexMatcher( query_, c_->indexKeyPattern() ) );
            if ( qp().exactKeyMatch() && ! matcher_->needRecord() ) {
                query_ = qp().simplifiedQuery( qp().indexKey() );
                bc_ = dynamic_cast< BtreeCursor* >( c_.get() );
                bc_->forgetEndKey();
            }
            
            skip_ = spec_["skip"].numberLong();
            limit_ = spec_["limit"].numberLong();
        }

        virtual void next() {
            if ( !c_->ok() ) {
                setComplete();
                return;
            }
            if ( bc_ ) {
                if ( firstMatch_.isEmpty() ) {
                    firstMatch_ = bc_->currKeyNode().key;
                    // if not match
                    if ( query_.woCompare( firstMatch_, BSONObj(), false ) ) {
                        setComplete();
                        return;
                    }
                    _gotOne();
                } else {
                    if ( !firstMatch_.woEqual( bc_->currKeyNode().key ) ) {
                        setComplete();
                        return;
                    }
                    _gotOne();
                }
            } else {
                if ( !matcher_->matches(c_->currKey(), c_->currLoc() ) ) {
                }
                else if( !c_->getsetdup(c_->currLoc()) ) {
                    _gotOne();
                }                
            }
            c_->advance();
        }
        virtual QueryOp *clone() const {
            return new CountOp( spec_ );
        }
        long long count() const { return count_; }
        virtual bool mayRecordPlan() const { return true; }
    private:
        
        void _gotOne(){
            if ( skip_ ){
                skip_--;
                return;
            }
            
            if ( limit_ > 0 && count_ >= limit_ ){
                setComplete();
                return;
            }

            count_++;
        }

        BSONObj spec_;
        long long count_;
        long long skip_;
        long long limit_;
        auto_ptr< Cursor > c_;
        BSONObj query_;
        BtreeCursor *bc_;
        auto_ptr< CoveredIndexMatcher > matcher_;
        BSONObj firstMatch_;
    };
    
    /* { count: "collectionname"[, query: <query>] }
       returns -1 on ns does not exist error.
    */    
    long long runCount( const char *ns, const BSONObj &cmd, string &err ) {
        NamespaceDetails *d = nsdetails( ns );
        if ( !d ) {
            err = "ns missing";
            return -1;
        }
        BSONObj query = cmd.getObjectField("query");

        // count of all objects
        if ( query.isEmpty() ){
            long long num = d->nrecords;
            num = num - cmd["skip"].numberLong();
            if ( num < 0 ) {
                num = 0;
            }
            if ( cmd["limit"].isNumber() ){
                long long limit = cmd["limit"].numberLong();
                if ( limit < num ){
                    num = limit;
                }
            }
            return num;
        }
        QueryPlanSet qps( ns, query, BSONObj() );
        CountOp original( cmd );
        shared_ptr< CountOp > res = qps.runOp( original );
        if ( !res->complete() ) {
            log() << "Count with ns: " << ns << " and query: " << query
                  << " failed with exception: " << res->exceptionMessage()
                  << endl;
            return 0;
        }
        return res->count();
    }

    // Implements database 'query' requests using the query optimizer's QueryOp interface
    class UserQueryOp : public QueryOp {
    public:
        UserQueryOp( int ntoskip, int ntoreturn, const BSONObj &order, bool wantMore,
                   bool explain, FieldMatcher *filter, int queryOptions ) :
            b_( 32768 ),
            ntoskip_( ntoskip ),
            ntoreturn_( ntoreturn ),
            order_( order ),
            wantMore_( wantMore ),
            explain_( explain ),
            filter_( filter ),
            ordering_(),
            nscanned_(),
            queryOptions_( queryOptions ),
            n_(),
            soSize_(),
            saveClientCursor_(),
            findingStart_( (queryOptions & QueryOption_OplogReplay) != 0 ),
            findingStartCursor_()
        {
            uassert( 10105 , "bad skip value in query", ntoskip >= 0);
        }

        virtual void init() {
            b_.skip( sizeof( QueryResult ) );
            
            // findingStart mode is used to find the first operation of interest when
            // we are scanning through a repl log.  For efficiency in the common case,
            // where the first operation of interest is closer to the tail than the head,
            // we start from the tail of the log and work backwards until we find the
            // first operation of interest.  Then we scan forward from that first operation,
            // actually returning results to the client.  During the findingStart phase,
            // we release the db mutex occasionally to avoid blocking the db process for
            // an extended period of time.
            if ( findingStart_ ) {
                // Use a ClientCursor here so we can release db mutex while scanning
                // oplog (can take quite a while with large oplogs).
                auto_ptr<Cursor> c = qp().newReverseCursor();
                findingStartCursor_ = new ClientCursor(c, qp().ns(), false);
            } else {
                c_ = qp().newCursor();
            }
            
            matcher_.reset(new CoveredIndexMatcher(qp().query(), qp().indexKey()));
            
            if ( qp().scanAndOrderRequired() ) {
                ordering_ = true;
                so_.reset( new ScanAndOrder( ntoskip_, ntoreturn_, order_ ) );
                wantMore_ = false;
            }
        }
        virtual void next() {
            if ( findingStart_ ) {
                if ( !findingStartCursor_ || !findingStartCursor_->c->ok() ) {
                    findingStart_ = false;
                    c_ = qp().newCursor();
                } else if ( !matcher_->matches( findingStartCursor_->c->currKey(), findingStartCursor_->c->currLoc() ) ) {
                    findingStart_ = false;
                    c_ = qp().newCursor( findingStartCursor_->c->currLoc() );
                } else {
                    findingStartCursor_->c->advance();
                    RARELY {
                        CursorId id = findingStartCursor_->cursorid;
                        findingStartCursor_->updateLocation();
                        {
                            dbtemprelease t;
                        }
                        findingStartCursor_ = ClientCursor::find( id, false );
                    }
                    return;
                }
            }
            
            if ( findingStartCursor_ ) {
                ClientCursor::erase( findingStartCursor_->cursorid );
                findingStartCursor_ = 0;
            }
            
            if ( !c_->ok() ) {
                finish();
                return;
            }
            
            bool mayCreateCursor1 = wantMore_ && ntoreturn_ != 1 && useCursors;
            
            if( 0 ) { 
                BSONObj js = c_->current();
                cout << "SCANNING " << js << endl;
            }

            nscanned_++;
            if ( !matcher_->matches(c_->currKey(), c_->currLoc() ) ) {
                ;
            }
            else {
                DiskLoc cl = c_->currLoc();
                if( !c_->getsetdup(cl) ) { 
                    BSONObj js = c_->current();
                    // got a match.
                    assert( js.objsize() >= 0 ); //defensive for segfaults
                    if ( ordering_ ) {
                        // note: no cursors for non-indexed, ordered results.  results must be fairly small.
                        so_->add(js);
                    }
                    else if ( ntoskip_ > 0 ) {
                        ntoskip_--;
                    } else {
                        if ( explain_ ) {
                            n_++;
                            if ( n_ >= ntoreturn_ && !wantMore_ ) {
                                // .limit() was used, show just that much.
                                finish();
                                return;
                            }
                        }
                        else {
                            fillQueryResultFromObj(b_, filter_, js);
                            n_++;
                            if ( (ntoreturn_>0 && (n_ >= ntoreturn_ || b_.len() > MaxBytesToReturnToClientAtOnce)) ||
                                 (ntoreturn_==0 && (b_.len()>1*1024*1024 || n_>=101)) ) {
                                /* if ntoreturn is zero, we return up to 101 objects.  on the subsequent getmore, there
                                   is only a size limit.  The idea is that on a find() where one doesn't use much results,
                                   we don't return much, but once getmore kicks in, we start pushing significant quantities.
                             
                                   The n limit (vs. size) is important when someone fetches only one small field from big
                                   objects, which causes massive scanning server-side.
                                */
                                /* if only 1 requested, no cursor saved for efficiency...we assume it is findOne() */
                                if ( mayCreateCursor1 ) {
                                    c_->advance();
                                    if ( c_->ok() ) {
                                        // more...so save a cursor
                                        saveClientCursor_ = true;
                                    }
                                }
                                finish();
                                return;
                                }
                        }
                    }
                }
            }
            c_->advance();            
        }
        void finish() {
            if ( explain_ ) {
                n_ = ordering_ ? so_->size() : n_;
            } else if ( ordering_ ) {
                so_->fill(b_, filter_, n_);
            }
            if ( mayCreateCursor2() ) {
                c_->setTailable();
            }
            // If the tailing request succeeded.
            if ( c_->tailable() ) {
                saveClientCursor_ = true;
            }
            setComplete();            
        }
        virtual bool mayRecordPlan() const { return ntoreturn_ != 1; }
        virtual QueryOp *clone() const {
            return new UserQueryOp( ntoskip_, ntoreturn_, order_, wantMore_, explain_, filter_, queryOptions_ );
        }
        BufBuilder &builder() { return b_; }
        bool scanAndOrderRequired() const { return ordering_; }
        auto_ptr< Cursor > cursor() { return c_; }
        auto_ptr< CoveredIndexMatcher > matcher() { return matcher_; }
        int n() const { return n_; }
        long long nscanned() const { return nscanned_; }
        bool saveClientCursor() const { return saveClientCursor_; }
        bool mayCreateCursor2() const { return ( queryOptions_ & QueryOption_CursorTailable ) && ntoreturn_ != 1; }
    private:
        BufBuilder b_;
        int ntoskip_;
        int ntoreturn_;
        BSONObj order_;
        bool wantMore_;
        bool explain_;
        FieldMatcher *filter_;   
        bool ordering_;
        auto_ptr< Cursor > c_;
        long long nscanned_;
        int queryOptions_;
        auto_ptr< CoveredIndexMatcher > matcher_;
        int n_;
        int soSize_;
        bool saveClientCursor_;
        auto_ptr< ScanAndOrder > so_;
        bool findingStart_;
        ClientCursor * findingStartCursor_;
    };
    
    /* run a query -- includes checking for and running a Command */
    auto_ptr< QueryResult > runQuery(Message& m, QueryMessage& q, CurOp& curop ) {
        StringBuilder& ss = curop.debug().str;
        const char *ns = q.ns;
        int ntoskip = q.ntoskip;
        int _ntoreturn = q.ntoreturn;
        BSONObj jsobj = q.query;
        auto_ptr< FieldMatcher > filter = q.fields; // what fields to return (unspecified = full object)
        int queryOptions = q.queryOptions;
        BSONObj snapshotHint;
        
        Timer t;
        if( logLevel >= 2 )
            log() << "runQuery: " << ns << jsobj << endl;
        
        long long nscanned = 0;
        bool wantMore = true;
        int ntoreturn = _ntoreturn;
        if ( _ntoreturn < 0 ) {
            /* _ntoreturn greater than zero is simply a hint on how many objects to send back per 
               "cursor batch".
               A negative number indicates a hard limit.
            */
            ntoreturn = -_ntoreturn;
            wantMore = false;
        }
        ss << "query " << ns << " ntoreturn:" << ntoreturn;
        curop.setQuery(jsobj);
        
        BufBuilder bb;
        BSONObjBuilder cmdResBuf;
        long long cursorid = 0;
        
        bb.skip(sizeof(QueryResult));
        
        auto_ptr< QueryResult > qr;
        int n = 0;
        
        Client& c = cc();
        /* we assume you are using findOne() for running a cmd... */
        if ( ntoreturn == 1 && runCommands(ns, jsobj, curop, bb, cmdResBuf, false, queryOptions) ) {
            n = 1;
            qr.reset( (QueryResult *) bb.buf() );
            bb.decouple();
            qr->setResultFlagsToOk();
            qr->len = bb.len();
            ss << " reslen:" << bb.len();
            //	qr->channel = 0;
            qr->setOperation(opReply);
            qr->cursorId = cursorid;
            qr->startingFrom = 0;
            qr->nReturned = n;            
        }
        else {
            /* regular query */
            
            AuthenticationInfo *ai = currentClient.get()->ai;
            uassert( 10106 , "unauthorized", ai->isAuthorized(c.database()->name.c_str()));

			/* we allow queries to SimpleSlave's -- but not to the slave (nonmaster) member of a replica pair 
			   so that queries to a pair are realtime consistent as much as possible.  use setSlaveOk() to 
			   query the nonmaster member of a replica pair.
			*/
            uassert( 10107 ,  "not master", isMaster() || (queryOptions & QueryOption_SlaveOk) || slave == SimpleSlave );

            BSONElement hint;
            BSONObj min;
            BSONObj max;
            bool explain = false;
            bool _gotquery = false;
            bool snapshot = false;
            BSONObj query;
            {
                BSONElement e = jsobj.findElement("$query");
                if ( e.eoo() )
                    e = jsobj.findElement("query");                    
                if ( !e.eoo() && (e.type() == Object || e.type() == Array) ) {
                    query = e.embeddedObject();
                    _gotquery = true;
                }
            }
            BSONObj order;
            {
                BSONElement e = jsobj.findElement("$orderby");
                if ( e.eoo() )
                    e = jsobj.findElement("orderby");                    
                if ( !e.eoo() ) {
                    order = e.embeddedObjectUserCheck();
                    if ( e.type() == Array )
                        order = transformOrderFromArrayFormat(order);
                }
            }
            if ( !_gotquery && order.isEmpty() )
                query = jsobj;
            else {
                explain = jsobj.getBoolField("$explain");
                if ( useHints )
                    hint = jsobj.getField("$hint");
                min = jsobj.getObjectField("$min");
                max = jsobj.getObjectField("$max");
                BSONElement e = jsobj.getField("$snapshot");
                snapshot = !e.eoo() && e.trueValue();
                if( snapshot ) { 
                    uassert( 12001 , "E12001 can't sort with $snapshot", order.isEmpty());
					uassert( 12002 , "E12002 can't use hint with $snapshot", hint.eoo());
                    NamespaceDetails *d = nsdetails(ns);
                    if ( d ){
                        int i = d->findIdIndex();
                        if( i < 0 ) { 
                            if ( strstr( ns , ".system." ) == 0 )
                                log() << "warning: no _id index on $snapshot query, ns:" << ns << endl;
                        }
                        else {
                            /* [dm] the name of an _id index tends to vary, so we build the hint the hard way here.
                               probably need a better way to specify "use the _id index" as a hint.  if someone is
                               in the query optimizer please fix this then!
                            */
                            BSONObjBuilder b;
                            b.append("$hint", d->idx(i).indexName());
                            snapshotHint = b.obj();
                            hint = snapshotHint.firstElement();
                        }
                    }
                }
            }
            
            /* The ElemIter will not be happy if this isn't really an object. So throw exception
               here when that is true.
               (Which may indicate bad data from client.)
            */
            if ( query.objsize() == 0 ) {
                out() << "Bad query object?\n  jsobj:";
                out() << jsobj.toString() << "\n  query:";
                out() << query.toString() << endl;
                uassert( 10110 , "bad query object", false);
            }
            
            bool idHackWorked = false;

            if ( strcmp( query.firstElement().fieldName() , "_id" ) == 0 && query.nFields() == 1 && query.firstElement().isSimpleType() ){
                nscanned = 1;

                bool nsFound = false;
                bool indexFound = false;

                BSONObj resObject;
                bool found = Helpers::findById( c, ns , query , resObject , &nsFound , &indexFound );
                if ( nsFound == false || indexFound == true ){
                    idHackWorked = true;
                    if ( found ){
                        n = 1;
                        fillQueryResultFromObj( bb , filter.get() , resObject );
                    }
                    qr.reset( (QueryResult *) bb.buf() );
                    bb.decouple();
                    qr->setResultFlagsToOk();
                    qr->len = bb.len();
                    ss << " reslen:" << bb.len();
                    qr->setOperation(opReply);
                    qr->cursorId = cursorid;
                    qr->startingFrom = 0;
                    qr->nReturned = n;       
                }     
            }
            
            if ( ! idHackWorked ){ // non-simple _id lookup
                BSONObj oldPlan;
                if ( explain && hint.eoo() && min.isEmpty() && max.isEmpty() ) {
                    QueryPlanSet qps( ns, query, order );
                    if ( qps.usingPrerecordedPlan() )
                        oldPlan = qps.explain();
                }
                QueryPlanSet qps( ns, query, order, &hint, !explain, min, max );
                UserQueryOp original( ntoskip, ntoreturn, order, wantMore, explain, filter.get(), queryOptions );
                shared_ptr< UserQueryOp > o = qps.runOp( original );
                UserQueryOp &dqo = *o;
                massert( 10362 ,  dqo.exceptionMessage(), dqo.complete() );
                n = dqo.n();
                nscanned = dqo.nscanned();
                if ( dqo.scanAndOrderRequired() )
                    ss << " scanAndOrder ";
                auto_ptr< Cursor > c = dqo.cursor();
                log( 5 ) << "   used cursor: " << c.get() << endl;
                if ( dqo.saveClientCursor() ) {
                    // the clientcursor now owns the Cursor* and 'c' is released:
                    ClientCursor *cc = new ClientCursor(c, ns, !(queryOptions & QueryOption_NoCursorTimeout));
                    cursorid = cc->cursorid;
                    cc->query = jsobj.getOwned();
                    DEV out() << "  query has more, cursorid: " << cursorid << endl;
                    cc->matcher = dqo.matcher();
                    cc->pos = n;
                    cc->filter = filter;
                    cc->originalMessage = m;
                    cc->updateLocation();
                    if ( !cc->c->ok() && cc->c->tailable() ) {
                        DEV out() << "  query has no more but tailable, cursorid: " << cursorid << endl;
                    } else {
                        DEV out() << "  query has more, cursorid: " << cursorid << endl;
                    }
                }
                if ( explain ) {
                    BSONObjBuilder builder;
                    builder.append("cursor", c->toString());
                    builder.append("startKey", c->prettyStartKey());
                    builder.append("endKey", c->prettyEndKey());
                    builder.append("nscanned", double( dqo.nscanned() ) );
                    builder.append("n", n);
                    if ( dqo.scanAndOrderRequired() )
                        builder.append("scanAndOrder", true);
                    builder.append("millis", t.millis());
                    if ( !oldPlan.isEmpty() )
                        builder.append( "oldPlan", oldPlan.firstElement().embeddedObject().firstElement().embeddedObject() );
                    if ( hint.eoo() )
                        builder.appendElements(qps.explain());
                    BSONObj obj = builder.done();
                    fillQueryResultFromObj(dqo.builder(), 0, obj);
                    n = 1;
                }
                qr.reset( (QueryResult *) dqo.builder().buf() );
                dqo.builder().decouple();
                qr->cursorId = cursorid;
                qr->setResultFlagsToOk();
                qr->len = dqo.builder().len();
                ss << " reslen:" << qr->len;
                qr->setOperation(opReply);
                qr->startingFrom = 0;
                qr->nReturned = n;
            }
        }
        
        int duration = t.millis();
        Database *database = c.database();
        if ( (database && database->profile) || duration >= 100 ) {
            ss << " nscanned:" << nscanned << ' ';
            if ( ntoskip )
                ss << " ntoskip:" << ntoskip;
            if ( database && database->profile )
                ss << " \nquery: ";
            ss << jsobj << ' ';
        }
        ss << " nreturned:" << n;
        return qr;        
    }    
    
} // namespace mongo
