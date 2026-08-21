// SPDX-License-Identifier: Apache-2.0

import { Box, Text } from 'ink';
import React from 'react';

import {
  type WorkbenchActionButton,
  type WorkbenchCheck,
  type WorkbenchFocus,
  type WorkbenchGuideOverlay,
  type WorkbenchLine,
  type WorkbenchNextPrompt,
  type WorkbenchReportDetail,
  type WorkbenchScrollBack,
  type WorkbenchSessionBuffers,
  boundedPromptRows,
  sessionTitleBar,
  workbenchViewportRows,
} from './session-workbench-state.js';
import {
  type TerminalDimensions,
  terminalCanvasRows,
} from './terminal-canvas.js';

function workbenchLineColor(tone: WorkbenchLine['tone']) {
  if (tone === 'running') return 'yellow';
  if (tone === 'good') return 'green';
  if (tone === 'bad') return 'red';
  if (tone === 'dim') return 'gray';
  return undefined;
}

function WorkbenchSessionPane({
  session,
  title,
  lines,
  active,
  scrollBack,
  viewportRows,
  running,
  titleBarColumns,
  activityFrame,
  footer,
}: {
  session: 1 | 2;
  title: string;
  lines: WorkbenchLine[];
  active: boolean;
  scrollBack: number;
  viewportRows: number;
  running: boolean;
  titleBarColumns: number;
  activityFrame: number;
  footer: string;
}) {
  const sessionLines = lines;
  const liveStart = Math.max(0, sessionLines.length - viewportRows);
  const start = Math.max(0, liveStart - scrollBack);
  const visible = sessionLines.slice(start, start + viewportRows);
  return (
    <Box
      width="50%"
      flexDirection="column"
      borderStyle="round"
      borderColor={active ? 'cyan' : 'gray'}
      overflow="hidden"
    >
      <Text
        bold
        color={active ? 'black' : 'white'}
        backgroundColor={active ? 'cyan' : 'gray'}
        wrap="truncate-end"
      >
        {sessionTitleBar({
          session,
          title,
          active,
          running,
          columns: titleBarColumns,
          activityFrame,
        })}
      </Text>
      <Box paddingX={1}>
        <Text dimColor>PUBLIC ACTIVITY · SENSITIVE INTERNALS HIDDEN</Text>
      </Box>
      <Box
        flexDirection="column"
        height={viewportRows}
        overflow="hidden"
        paddingX={1}
      >
        {visible.length === 0 ? (
          <Text dimColor>Activity will appear one event at a time.</Text>
        ) : null}
        {visible.map((line, index) => (
          <Text
            key={`${start + index}-${line.source}-${line.text}`}
            color={workbenchLineColor(line.tone)}
            wrap="truncate-end"
          >
            {String(start + index + 1).padStart(2, '0')}{' '}
            {line.source.padEnd(11)} {line.text}
          </Text>
        ))}
      </Box>
      <Box paddingX={1}>
        <Text dimColor>{footer}</Text>
      </Box>
    </Box>
  );
}

function WorkbenchReportDetailPanel({
  dimensions,
  checks,
  detail,
  caption,
  interactive,
}: {
  dimensions: TerminalDimensions;
  checks: WorkbenchCheck[];
  detail: WorkbenchReportDetail;
  caption: string;
  interactive: boolean;
}) {
  const rows = checks.filter(
    (check) => (detail === 'correct') === check.passed,
  );
  const correct = detail === 'correct';
  return (
    <Box
      width={dimensions.columns}
      height={terminalCanvasRows(dimensions.rows)}
      flexDirection="column"
      borderStyle="double"
      borderColor={correct ? 'green' : 'red'}
      paddingX={1}
      overflow="hidden"
    >
      <Text bold color={correct ? 'green' : 'red'}>
        {interactive
          ? `${correct ? '✓ CORRECT CHECKS' : '× FAILED CHECKS'} · ${rows.length}`
          : correct
            ? `✓ ACCEPTANCE REPORT · ${rows.length}/${checks.length} CHECKS PASSED`
            : `× ACCEPTANCE REPORT · ${rows.length} FAILED CHECKS`}
      </Text>
      <Text dimColor>{caption}</Text>
      <Box flexDirection="column" flexGrow={1} minHeight={0} marginTop={1}>
        {rows.length === 0 ? (
          <Text color={correct ? 'yellow' : 'green'}>
            {correct
              ? 'No correct checks were recorded.'
              : 'No failed checks. This is the expected result.'}
          </Text>
        ) : null}
        {rows.map((row, index) => (
          <Box key={row.id} flexDirection="column" marginBottom={1}>
            <Text bold color={row.passed ? 'green' : 'red'}>
              {String(index + 1).padStart(2, '0')} {row.passed ? '✓' : '×'}{' '}
              {row.title}
            </Text>
            <Text dimColor>{row.meaning}</Text>
          </Box>
        ))}
      </Box>
      <Box borderStyle="round" borderColor="cyan" paddingX={1}>
        <Text bold color="cyan" wrap="truncate-end">
          {interactive
            ? '← RETURN TO RESULT CARDS · Esc / Enter / Backspace / b'
            : 'DEMO COMPLETE · This acceptance report closes automatically'}
        </Text>
      </Box>
    </Box>
  );
}

function WorkbenchResultCard({
  kind,
  count,
  active,
  available,
  emphasized,
  interactive,
}: {
  kind: WorkbenchReportDetail;
  count: number;
  active: boolean;
  available: boolean;
  emphasized: boolean;
  interactive: boolean;
}) {
  const correct = kind === 'correct';
  const tone = correct || count === 0 ? 'green' : 'red';
  const cardColor = !available ? 'gray' : active || emphasized ? 'cyan' : tone;
  return (
    <Box
      width="50%"
      height={3}
      borderStyle={emphasized ? 'double' : 'round'}
      borderColor={cardColor}
      paddingX={1}
      overflow="hidden"
    >
      <Text bold color={cardColor} wrap="truncate-end">
        {active ? '> ' : '  '}
        {correct ? '✓' : '×'} {count} {correct ? 'CORRECT' : 'FAILED'}
        {available
          ? interactive
            ? ' · click / Enter details'
            : ' · verified'
          : ' · waiting'}
      </Text>
    </Box>
  );
}

function opaqueWorkbenchLine(value: string, columns: number): string {
  return value.slice(0, columns).padEnd(columns);
}

export type SessionWorkbenchProps = {
  dimensions: TerminalDimensions;
  heading: string;
  collectionLabel: string;
  caseLabel: string;
  relationship: string;
  controls: string;
  controlActions?: WorkbenchActionButton[];
  help: string;
  sourceLabel: string;
  targetLabel: string;
  buffers: WorkbenchSessionBuffers;
  checks: WorkbenchCheck[];
  reportAvailable: boolean;
  reportPassed: boolean;
  verdictSuccess: string;
  verdictFailure: string;
  verdictDetail?: string;
  detailCaption: string;
  busy: string;
  progress: string;
  error: string;
  activeFocus: WorkbenchFocus;
  scrollBack: WorkbenchScrollBack;
  showHelp: boolean;
  activityFrame: number;
  runningSession?: 1 | 2;
  nextPrompt?: WorkbenchNextPrompt;
  guideOverlay?: WorkbenchGuideOverlay;
  reportDetail?: WorkbenchReportDetail;
  emphasizedResult?: WorkbenchReportDetail;
  interactive?: boolean;
};

type ResolvedSessionWorkbenchProps = Omit<
  SessionWorkbenchProps,
  'interactive'
> & {
  interactive: boolean;
};
type ResolvedWorkbenchComponentProps = {
  workbench: ResolvedSessionWorkbenchProps;
};

function WorkbenchHeader(props: ResolvedWorkbenchComponentProps) {
  const { workbench } = props;
  const {
    heading,
    collectionLabel,
    caseLabel,
    relationship,
    controls,
    controlActions,
    help,
    sourceLabel,
    targetLabel,
    showHelp,
  } = workbench;
  return (
    <>
      <Box paddingX={1} justifyContent="space-between">
        <Text bold color="cyan">
          {heading.toUpperCase()}
        </Text>
        <Text>
          {collectionLabel} · {caseLabel}
        </Text>
      </Box>
      <Text wrap="truncate-end">
        S1 {sourceLabel} {relationship} S2 {targetLabel}
      </Text>
      <Text dimColor wrap="truncate-end">
        {controlActions?.map((action, index) => (
          <React.Fragment key={action.action}>
            {index > 0 ? ' ' : null}
            <Text bold color="cyan">
              [ {action.label} ]
            </Text>
          </React.Fragment>
        ))}
        {controlActions?.length ? ' · ' : null}
        {controls}
      </Text>
      {showHelp ? (
        <Text dimColor wrap="truncate-end">
          {help}
        </Text>
      ) : null}
    </>
  );
}

function workbenchSessionFooter(
  workbench: ResolvedSessionWorkbenchProps,
  session: 1 | 2,
): string {
  if (!workbench.interactive) {
    return 'Following admitted public activity one event at a time.';
  }
  const position =
    workbench.scrollBack[session] > 0
      ? `${workbench.scrollBack[session]} lines back`
      : 'following live';
  return `click focus · wheel here / ↑↓ scroll · Tab switch · ${position}`;
}

function WorkbenchSessions(props: ResolvedWorkbenchComponentProps) {
  const { workbench } = props;
  const {
    dimensions,
    sourceLabel,
    targetLabel,
    buffers,
    activeFocus,
    scrollBack,
    showHelp,
    verdictDetail,
    progress,
    runningSession,
    activityFrame,
  } = workbench;
  const titleColumns = Math.max(1, Math.floor(dimensions.columns / 2) - 2);
  const viewportRows = workbenchViewportRows({
    dimensions,
    showHelp,
    verdictDetail,
  });
  return (
    <Box flexGrow={1} minHeight={0}>
      <WorkbenchSessionPane
        session={1}
        title={sourceLabel}
        lines={buffers[1]}
        active={activeFocus === 'session-1'}
        scrollBack={scrollBack[1]}
        viewportRows={viewportRows}
        running={Boolean(progress) && runningSession === 1}
        titleBarColumns={titleColumns}
        activityFrame={activityFrame}
        footer={workbenchSessionFooter(workbench, 1)}
      />
      <WorkbenchSessionPane
        session={2}
        title={targetLabel}
        lines={buffers[2]}
        active={activeFocus === 'session-2'}
        scrollBack={scrollBack[2]}
        viewportRows={viewportRows}
        running={Boolean(progress) && runningSession === 2}
        titleBarColumns={titleColumns}
        activityFrame={activityFrame}
        footer={workbenchSessionFooter(workbench, 2)}
      />
    </Box>
  );
}

type WorkbenchVerdictState = {
  color: 'green' | 'red';
  headline: string;
  passedCount: number;
  failedCount: number;
};

function resolveWorkbenchVerdictState(
  workbench: ResolvedSessionWorkbenchProps,
): WorkbenchVerdictState {
  const {
    checks,
    reportAvailable,
    reportPassed,
    verdictSuccess,
    verdictFailure,
    error,
    busy,
    progress,
  } = workbench;
  const passedCount = checks.filter((check) => check.passed).length;
  const failedCount = checks.length - passedCount;
  if (!reportAvailable) {
    return {
      color: 'red',
      headline: error || busy || progress || 'Ready · choose a test case',
      passedCount,
      failedCount,
    };
  }
  return {
    color: reportPassed ? 'green' : 'red',
    headline: reportPassed ? verdictSuccess : verdictFailure,
    passedCount,
    failedCount,
  };
}

type WorkbenchVerdictComponentProps = ResolvedWorkbenchComponentProps & {
  verdict: WorkbenchVerdictState;
};

function WorkbenchVerdictMessage(props: WorkbenchVerdictComponentProps) {
  const { workbench, verdict } = props;
  return (
    <>
      <Text
        color={workbench.reportAvailable ? verdict.color : 'yellow'}
        bold
        wrap="truncate-end"
      >
        {verdict.headline}
      </Text>
      {workbench.reportAvailable && workbench.verdictDetail ? (
        <Text color={verdict.color} bold wrap="truncate-end">
          {workbench.verdictDetail}
        </Text>
      ) : null}
    </>
  );
}

function WorkbenchVerdictCards(props: WorkbenchVerdictComponentProps) {
  const { workbench, verdict } = props;
  return (
    <Box>
      <WorkbenchResultCard
        kind="correct"
        count={verdict.passedCount}
        active={workbench.interactive && workbench.activeFocus === 'correct'}
        available={workbench.reportAvailable}
        emphasized={workbench.emphasizedResult === 'correct'}
        interactive={workbench.interactive}
      />
      <WorkbenchResultCard
        kind="failed"
        count={verdict.failedCount}
        active={workbench.interactive && workbench.activeFocus === 'failed'}
        available={workbench.reportAvailable}
        emphasized={workbench.emphasizedResult === 'failed'}
        interactive={workbench.interactive}
      />
    </Box>
  );
}

function WorkbenchVerdictDock(props: ResolvedWorkbenchComponentProps) {
  const { workbench } = props;
  const verdict = resolveWorkbenchVerdictState(workbench);
  return (
    <Box
      height={workbench.verdictDetail ? 7 : 6}
      borderStyle="round"
      borderColor={workbench.reportAvailable ? verdict.color : 'gray'}
      paddingX={1}
      flexDirection="column"
      overflow="hidden"
    >
      <WorkbenchVerdictMessage workbench={workbench} verdict={verdict} />
      <WorkbenchVerdictCards workbench={workbench} verdict={verdict} />
    </Box>
  );
}

function WorkbenchNextPromptPanel(props: ResolvedWorkbenchComponentProps) {
  const { workbench } = props;
  const { dimensions, nextPrompt } = workbench;
  if (!nextPrompt) return null;
  const width = Math.min(
    dimensions.columns,
    Math.min(68, Math.max(24, dimensions.columns - 8)),
  );
  const columns = Math.max(1, width - 2);
  const rows = boundedPromptRows(
    `${nextPrompt.title} · ${nextPrompt.instruction}`,
    Math.max(1, columns - 2),
  );
  return (
    <Box
      position="absolute"
      width={width}
      height={6}
      marginTop={Math.max(4, Math.floor(dimensions.rows / 2) - 2)}
      marginLeft={Math.max(2, Math.floor((dimensions.columns - width) / 2))}
      borderStyle="double"
      borderColor="yellow"
      flexDirection="column"
      overflow="hidden"
    >
      <Text bold color="yellow" backgroundColor="blue">
        {opaqueWorkbenchLine(' WHAT TO TRY NEXT', columns)}
      </Text>
      {rows.map((row, index) => (
        <Text key={`${index}-${row}`} color="white" backgroundColor="blue">
          {opaqueWorkbenchLine(` ${row}`, columns)}
        </Text>
      ))}
      <Text color="white" backgroundColor="blue">
        {opaqueWorkbenchLine(' Closes automatically in 5 seconds.', columns)}
      </Text>
    </Box>
  );
}

function WorkbenchGuideOverlayPanel(props: ResolvedWorkbenchComponentProps) {
  const { workbench } = props;
  const { dimensions, guideOverlay } = workbench;
  if (!guideOverlay) return null;
  const width = Math.min(
    dimensions.columns,
    Math.min(88, Math.max(32, dimensions.columns - 8)),
  );
  const columns = Math.max(1, width - 2);
  const rows = guideOverlay.lines.flatMap((line) =>
    boundedPromptRows(line, Math.max(1, columns - 2)),
  );
  return (
    <Box
      position="absolute"
      width={width}
      height={Math.min(
        terminalCanvasRows(dimensions.rows) - 4,
        rows.length + 5,
      )}
      marginTop={Math.max(
        2,
        Math.floor((dimensions.rows - rows.length - 5) / 2),
      )}
      marginLeft={Math.max(2, Math.floor((dimensions.columns - width) / 2))}
      borderStyle="double"
      borderColor="cyan"
      flexDirection="column"
      overflow="hidden"
    >
      <Text bold color="black" backgroundColor="cyan">
        {opaqueWorkbenchLine(` ${guideOverlay.heading}`, columns)}
      </Text>
      <Text bold color="cyan" backgroundColor="black">
        {opaqueWorkbenchLine(` ${guideOverlay.title}`, columns)}
      </Text>
      {rows.map((row, index) => (
        <Text key={`${index}-${row}`} color="white" backgroundColor="black">
          {opaqueWorkbenchLine(` ${row}`, columns)}
        </Text>
      ))}
      <Text bold color="cyan" backgroundColor="black">
        {opaqueWorkbenchLine(` ${guideOverlay.footer}`, columns)}
      </Text>
    </Box>
  );
}

export function SessionWorkbenchView(props: SessionWorkbenchProps) {
  const workbench: ResolvedSessionWorkbenchProps = {
    ...props,
    interactive: props.interactive ?? true,
  };
  if (workbench.reportAvailable && workbench.reportDetail) {
    return (
      <WorkbenchReportDetailPanel
        dimensions={workbench.dimensions}
        checks={workbench.checks}
        detail={workbench.reportDetail}
        caption={workbench.detailCaption}
        interactive={workbench.interactive}
      />
    );
  }
  return (
    <Box
      width={workbench.dimensions.columns}
      height={terminalCanvasRows(workbench.dimensions.rows)}
      flexDirection="column"
      overflow="hidden"
    >
      <WorkbenchHeader workbench={workbench} />
      <WorkbenchSessions workbench={workbench} />
      <WorkbenchVerdictDock workbench={workbench} />
      <WorkbenchNextPromptPanel workbench={workbench} />
      <WorkbenchGuideOverlayPanel workbench={workbench} />
    </Box>
  );
}
