-- ---------------------------------------------------------------
--
-- Clockwork Agent Database Schema
--
-- This schema definition is implemented in the agent (cwa) local
-- data store for keeping track of it's own individual runs and
-- compliance scores.
--
-- This data is also submitted to the policy master for aggregate
-- reporting across a multi-host implementation.
--
-- This schema works with SQLite's DDL
--
-- ---------------------------------------------------------------


--
-- stats - Singleton table for storing local statistics.
--
-- Note: This table does not have a counterpart in the
--       policy master reporting database.
--
create table stats (
  last_run_at           DATETIME, -- Used to spread out checks
  next_run_after        DATETIME, -- Used for cwa in daemon mode?
  last_policy_version   INTEGER,
  num_failures          INTEGER -- Safety net for bad policy?
);

--
-- jobs - Each time cwa runs, it creates a new job record.
--
create table jobs (
  id              INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT,
  started_at      DATETIME,
  ended_at        DATETIME,
  duration        INTEGER,
  result          VARCHAR(20)
);

--
-- resources - Every job has multiple resources attached.
--
create table resources (
  id              INTEGER NOT NULL PRIMARY KEY,
  job_id          INTEGER,
  restype         VARCHAR(30),
  name            VARCHAR(100),
  sequence        INTEGER,
  result          VARCHAR(20)
);

--
-- actions - Each resource may have multiple actions attached.
--
create table actions (
  resource_id     INTEGER,
  summary         TEXT,
  sequence        INTEGER,
  result          VARCHAR(20)
);
