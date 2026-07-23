package com.jiran.jdoc;

/** Thrown when a native JDoc call fails. */
public class JdocException extends RuntimeException {
    public JdocException(String message) {
        super(message);
    }
}
