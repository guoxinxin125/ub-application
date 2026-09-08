//go:build !ubsm

package main

func openServiceState() error     { return nil }
func closeServiceState()          {}
func validateServiceState() error { return nil }
