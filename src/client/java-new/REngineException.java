// REngine - generic Java/R API
//
// Copyright (C) 2006 Simon Urbanek
// --- for licensing information see LICENSE file in the original JRclient distribution ---
//
//  RSrvException.java
//
//  Created by Simon Urbanek on Wed Jun 21 2006.
//
//  $Id: REngineException.java 2555 2006-06-21 20:36:42Z urbaneks $
//

package org.rosuda.REngine;

public class REngineException extends Exception {
    protected REngine engine;

    public REngineException(REngine engine, String msg) {
        super(msg);
        this.engine = engine;
    }
}
