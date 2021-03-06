Random.setRandomSeed();

t = db.test_index_check9;

function doIt() {

t.drop();

function sort() {
    var sort = {};
    for( var i = 0; i < n; ++i ) {
        sort[ fields[ i ] ] = Random.rand() > 0.5 ? 1 : -1;
    }    
    return sort;
}

var fields = [ 'a', 'b', 'c', 'd', 'e' ];
n = Random.randInt( 5 ) + 1;
var idx = sort();

t.ensureIndex( idx );

function obj() {
    var ret = {};
    for( var i = 0; i < n; ++i ) {
        ret[ fields[ i ] ] = r();
    }
    return ret;
}

function r() {
    return Random.randInt( 10 );
}

function check() {
    var v = t.validate();
    if ( !t.valid ) {
        printjson( t );
        assert( t.valid );
    }
    var spec = {};
    for( var i = 0; i < n; ++i ) {
        if ( Random.rand() > 0.5 ) {
            var bounds = [ r(), r() ];
            bounds.sort();
	    var s = {};
	    if ( Random.rand() > 0.5 ) {
		s[ "$gte" ] = bounds[ 0 ];
	    } else {
		s[ "$gt" ] = bounds[ 0 ];
	    }
	    if ( Random.rand() > 0.5 ) {
		s[ "$lte" ] = bounds[ 1 ];
	    } else {
		s[ "$lt" ] = bounds[ 1 ];
	    }
            spec[ fields[ i ] ] = s;
        } else {
            var vals = []
            for( var j = 0; j < Random.randInt( 15 ); ++j ) {
                vals.push( r() );
            }
            spec[ fields[ i ] ] = { $in: vals };
        }
    }
    s = sort();
    c1 = t.find( spec, { _id:null } ).sort( s ).hint( idx ).toArray();
    c2 = t.find( spec ).sort( s ).explain().nscanned;
    c3 = t.find( spec, { _id:null } ).sort( s ).hint( {$natural:1} ).toArray();
    //    assert.eq( c1, c3, "spec: " + tojson( spec ) + ", sort: " + tojson( s ) );
    //    assert.eq( c1.length, c2 );
    assert.eq( c1, c3 );
}

for( var i = 0; i < 10000; ++i ) {
    t.save( obj() );
    if( Random.rand() > 0.999 ) {
        print( i );
        check();
    }
}

for( var i = 0; i < 100000; ++i ) {
    if ( Random.rand() > 0.9 ) {
        t.save( obj() );
    } else {
        t.remove( obj() ); // improve
    }
    if( Random.rand() > 0.999 ) {
        print( i );
        check();
    }
}

check();

}

for( var z = 0; z < 5; ++z ) {
    doIt();
}